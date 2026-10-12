// tests/networking_tests/test_networking.cpp
// Integration test: spins up a TaskServer on localhost, sends a task,
// and verifies the echo (server echoes the task_id back).
// No GPU required.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")

#include "networking/tcp_connection.hpp"
#include "networking/health_monitor.hpp"
#include "protocol/message.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace dgpu;
using namespace dgpu::protocol;
using namespace dgpu::networking;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::cerr << "FAIL [" __FILE__ ":" << __LINE__ << "] " #expr << '\n'; \
            ++g_failed;                                                      \
        } else {                                                             \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

// ─── Echo server handler ──────────────────────────────────────────────────────

static void echoHandler(TcpConnection conn) {
    Message msg;
    while (conn.receiveMessage(msg)) {
        // Reflect the same message back so the client can correlate task IDs
        conn.sendMessage(msg);
    }
}

// ─── Test: ping-pong ──────────────────────────────────────────────────────────

static void test_ping_pong() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    TaskServer server;
    CHECK(server.start(17777, echoHandler));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpConnection client;
    CHECK(client.connect("127.0.0.1", 17777));

    auto ping = Message::makePing(123);
    CHECK(client.sendMessage(ping));

    Message pong;
    CHECK(client.receiveMessage(pong));

    // Verify magic echoed correctly (still in network byte order)
    uint32_t magic = 0;
    std::memcpy(&magic, &pong.header.magic, sizeof(magic));
    CHECK(_byteswap_ulong(magic) == MAGIC);

    client.close();
    server.stop();
    std::cout << "  ping-pong: PASS\n";
}

// ─── Test: malformed header and bad magic over real socket ───────────────────

static void test_bad_magic() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    std::atomic<bool> serverDetectedError{false};
    TaskServer server;
    CHECK(server.start(17778, [&](TcpConnection conn) {
        Message msg;
        // receiveMessage should reject the frame and return false
        bool ok = conn.receiveMessage(msg);
        if (!ok) serverDetectedError = true;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Connect using raw Winsock to inject byte-level corrupt frame
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(sock != INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(17778);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    CHECK(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Send a header with invalid magic (0xDEADBEEF instead of 0x44475055)
    MessageHeader badHdr{};
    badHdr.magic       = htonl(0xDEADBEEFu);
    badHdr.version     = htons(PROTO_VERSION);
    badHdr.messageType = htons(static_cast<uint16_t>(MessageType::Ping));
    badHdr.taskId      = 0;
    badHdr.payloadSize = 0;
    badHdr.status      = 0;

    int sent = ::send(sock, reinterpret_cast<const char*>(&badHdr), sizeof(badHdr), 0);
    CHECK(sent == sizeof(badHdr));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(serverDetectedError.load());

    ::closesocket(sock);
    server.stop();
    std::cout << "  bad-magic (live socket rejection): PASS\n";
}

// ─── Test: oversized payload rejection over real socket ───────────────────────

static void test_oversized_payload() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    std::atomic<bool> serverRejectedOversized{false};
    TaskServer server;
    CHECK(server.start(17780, [&](TcpConnection conn) {
        Message msg;
        // payload size > 64MB must be rejected
        bool ok = conn.receiveMessage(msg);
        if (!ok) serverRejectedOversized = true;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(sock != INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(17780);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    CHECK(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Valid magic, but payload size = 128 MB (exceeds 64 MB MAX_PAYLOAD)
    MessageHeader hugeHdr{};
    hugeHdr.magic       = htonl(MAGIC);
    hugeHdr.version     = htons(PROTO_VERSION);
    hugeHdr.messageType = htons(static_cast<uint16_t>(MessageType::Ping));
    hugeHdr.taskId      = 0;
    hugeHdr.payloadSize = htonl(128 * 1024 * 1024);
    hugeHdr.status      = 0;

    int sent = ::send(sock, reinterpret_cast<const char*>(&hugeHdr), sizeof(hugeHdr), 0);
    CHECK(sent == sizeof(hugeHdr));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(serverRejectedOversized.load());

    ::closesocket(sock);
    server.stop();
    std::cout << "  oversized-payload (>64MB live rejection): PASS\n";
}

// ─── Test: truncated stream / unexpected EOF ─────────────────────────────────

static void test_truncated_stream() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    std::atomic<bool> serverHandledEof{false};
    TaskServer server;
    CHECK(server.start(17781, [&](TcpConnection conn) {
        Message msg;
        bool ok = conn.receiveMessage(msg);
        if (!ok) serverHandledEof = true;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(sock != INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(17781);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    CHECK(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    // Send only 8 bytes of header, then abruptly close socket
    uint32_t partial[2] = { htonl(MAGIC), htons(PROTO_VERSION) };
    ::send(sock, reinterpret_cast<const char*>(partial), sizeof(partial), 0);
    ::closesocket(sock);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(serverHandledEof.load());

    server.stop();
    std::cout << "  truncated-stream (EOF handling): PASS\n";
}

// ─── Test: large payload ──────────────────────────────────────────────────────

static void test_large_payload() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    TaskServer server;
    CHECK(server.start(17779, echoHandler));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpConnection client;
    CHECK(client.connect("127.0.0.1", 17779));

    // Build a 1 MB payload
    std::vector<std::byte> bigPayload(1024 * 1024);
    for (size_t i = 0; i < bigPayload.size(); ++i) {
        bigPayload[i] = static_cast<std::byte>(i & 0xFF);
    }

    Message msg;
    msg.header.magic       = _byteswap_ulong(MAGIC);
    msg.header.version     = _byteswap_ushort(PROTO_VERSION);
    msg.header.messageType = _byteswap_ushort(
        static_cast<uint16_t>(MessageType::Ping));
    msg.header.taskId      = _byteswap_uint64(999ull);
    msg.header.payloadSize = _byteswap_ulong(
        static_cast<uint32_t>(bigPayload.size()));
    msg.header.status      = 0;
    msg.payload            = bigPayload;

    CHECK(client.sendMessage(msg));

    Message echo;
    CHECK(client.receiveMessage(echo));
    CHECK(echo.payload == bigPayload);

    client.close();
    server.stop();
    std::cout << "  large-payload (1MB): PASS\n";
}

// ─── Test: worker capabilities negotiation & pre-dispatch rejection ─────────

static void test_worker_capabilities_negotiation() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    WorkerCapabilities advertisedCaps;
    advertisedCaps.workerId       = "uuid-laptop-b-1650ti";
    advertisedCaps.hostname       = "Laptop-B";
    advertisedCaps.gpuName        = "GTX 1650 Ti";
    advertisedCaps.vramBytes      = 4ull * 1024ull * 1024ull * 1024ull;
    advertisedCaps.supportedTasks = TaskCap_Blur; // Only Blur supported, NOT EdgeDetection!
    advertisedCaps.maxWidth       = 1920;
    advertisedCaps.maxHeight      = 1080;

    TaskServer server;
    CHECK(server.start(17782, [advertisedCaps](TcpConnection conn) {
        // Handshake
        Message hello;
        if (!conn.receiveMessage(hello)) return;

        Message ack = Message::makeHelloAck("Laptop-B", "GTX 1650 Ti", 4096, advertisedCaps);
        if (!conn.sendMessage(ack)) return;

        Message msg;
        while (conn.receiveMessage(msg)) {
            uint16_t msgType = 0;
            std::memcpy(&msgType, &msg.header.messageType, sizeof(msgType));
            msgType = _byteswap_ushort(msgType);

            if (msgType == static_cast<uint16_t>(MessageType::Ping)) {
                uint64_t tid = 0;
                std::memcpy(&tid, &msg.header.taskId, sizeof(tid));
                auto pong = Message::makePong(_byteswap_uint64(tid));
                conn.sendMessage(pong);
                continue;
            }

            if (msgType == static_cast<uint16_t>(MessageType::SubmitTask)) {
                // Echo successful dummy result
                uint64_t tid = 0;
                std::memcpy(&tid, &msg.header.taskId, sizeof(tid));
                tid = _byteswap_uint64(tid);
                TaskResult res;
                res.taskId = tid;
                res.status = TaskStatus::Ok;
                Message resp;
                resp.header.magic       = _byteswap_ulong(MAGIC);
                resp.header.version     = _byteswap_ushort(PROTO_VERSION);
                resp.header.messageType = _byteswap_ushort(static_cast<uint16_t>(MessageType::TaskResult));
                resp.header.taskId      = _byteswap_uint64(tid);
                auto pl = res.serialise();
                resp.header.payloadSize = _byteswap_ulong(static_cast<uint32_t>(pl.size()));
                resp.header.status      = 0;
                resp.payload            = std::move(pl);
                conn.sendMessage(resp);
            }
        }
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TaskClient client;
    CHECK(client.connect("127.0.0.1", 17782));
    CHECK(client.hasCapabilities());
    CHECK(client.capabilities().workerId == "uuid-laptop-b-1650ti");
    CHECK(client.capabilities().maxWidth == 1920);

    // 1. Submit supported task (Blur, 1280x720) -> should succeed
    TaskRequest validReq;
    validReq.taskId = 101;
    validReq.type   = TaskType::Blur;
    validReq.width  = 1280;
    validReq.height = 720;
    validReq.input.resize(1280 * 720 * 4, std::byte{0});
    client.setCompressionEnabled(false);
    auto res1 = client.submit(validReq);
    CHECK(res1.status == TaskStatus::Ok);

    // 2. Submit unsupported task (EdgeDetection) -> pre-dispatch rejected immediately!
    TaskRequest unsupportedReq;
    unsupportedReq.taskId = 102;
    unsupportedReq.type   = TaskType::EdgeDetection;
    unsupportedReq.width  = 1280;
    unsupportedReq.height = 720;
    unsupportedReq.input.resize(1280 * 720 * 4, std::byte{0});
    auto res2 = client.submit(unsupportedReq);
    CHECK(res2.status == TaskStatus::UnsupportedTask);

    // 3. Submit oversized resolution (3840x2160 > maxWidth 1920) -> pre-dispatch rejected!
    TaskRequest oversizedReq;
    oversizedReq.taskId = 103;
    oversizedReq.type   = TaskType::Blur;
    oversizedReq.width  = 3840;
    oversizedReq.height = 2160;
    oversizedReq.input.resize(3840 * 2160 * 4, std::byte{0});
    auto res3 = client.submit(oversizedReq);
    CHECK(res3.status == TaskStatus::UnsupportedTask);

    client.disconnect();
    server.stop();
    std::cout << "  worker-capabilities-negotiation & rejection: PASS\n";
}

// ─── Test: worker health monitor state transitions & reconnection ─────────────

static void test_health_monitor_state_transitions() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    auto makeEchoHandler = []() {
        return [](TcpConnection conn) {
            Message msg;
            while (conn.receiveMessage(msg)) {
                uint16_t msgType = 0;
                std::memcpy(&msgType, &msg.header.messageType, sizeof(msgType));
                msgType = _byteswap_ushort(msgType);
                if (msgType == static_cast<uint16_t>(MessageType::Hello)) {
                    auto ack = Message::makeHelloAck("WorkerB", "MockGPU", 4096);
                    conn.sendMessage(ack);
                } else if (msgType == static_cast<uint16_t>(MessageType::Ping)) {
                    uint64_t tid = 0;
                    std::memcpy(&tid, &msg.header.taskId, sizeof(tid));
                    auto pong = Message::makePong(_byteswap_uint64(tid));
                    conn.sendMessage(pong);
                }
            }
        };
    };

    auto server = std::make_unique<TaskServer>();
    CHECK(server->start(17783, makeEchoHandler()));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TaskClient client;
    CHECK(client.connect("127.0.0.1", 17783));

    HealthMonitorConfig cfg;
    cfg.heartbeatIntervalMs = 50;  // Fast heartbeat for test
    cfg.heartbeatTimeoutMs  = 100;
    cfg.suspectThreshold    = 1;
    cfg.disconnectThreshold = 2;
    cfg.initialBackoffMs    = 50;
    cfg.maxBackoffMs        = 200;
    cfg.autoReconnect       = true;

    std::vector<WorkerHealthState> history;
    std::mutex histMu;

    WorkerHealthMonitor monitor(client, "127.0.0.1", 17783, cfg);
    monitor.setStateCallback([&](WorkerHealthState /*oldS*/, WorkerHealthState newS) {
        std::lock_guard<std::mutex> lk(histMu);
        history.push_back(newS);
    });

    monitor.start();
    CHECK(monitor.isHealthy());

    // Allow a few successful heartbeats
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(monitor.successfulHeartbeats() > 0);

    // Stop server to simulate worker crash
    server->stop();

    // Wait for monitor to transition Healthy -> Suspect -> Disconnected -> Recovering
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    {
        std::lock_guard<std::mutex> lk(histMu);
        bool sawSuspect      = false;
        bool sawDisconnected = false;
        bool sawRecovering   = false;
        for (auto s : history) {
            if (s == WorkerHealthState::Suspect)      sawSuspect = true;
            if (s == WorkerHealthState::Disconnected) sawDisconnected = true;
            if (s == WorkerHealthState::Recovering)   sawRecovering = true;
        }
        CHECK(sawSuspect);
        CHECK(sawDisconnected);
        CHECK(sawRecovering);
    }

    // Now restart worker server on same port — monitor should auto-reconnect!
    server = std::make_unique<TaskServer>();
    CHECK(server->start(17783, makeEchoHandler()));

    // Wait for auto-reconnect back to Healthy
    bool reconnected = false;
    for (int i = 0; i < 60; ++i) {
        if (monitor.isHealthy()) {
            reconnected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    CHECK(reconnected);
    CHECK(monitor.reconnectAttempts() > 0);

    monitor.stop();
    client.disconnect();
    server->stop();
    std::cout << "  health-monitor state transitions & auto-recovery: PASS\n";
}

// ─── Test: Adaptive compression and transfer policy with TaskClient ───────────

static void test_adaptive_transfer_policy_client() {
    WinsockInit wsa;
    CHECK(wsa.ok());

    TaskServer server;
    CHECK(server.start(17784, [](TcpConnection conn) {
        Message hello;
        if (!conn.receiveMessage(hello)) return;
        auto ack = Message::makeHelloAck("WorkerAdaptive", "RTX 3050", 4096);
        if (!conn.sendMessage(ack)) return;

        Message msg;
        while (conn.receiveMessage(msg)) {
            uint16_t msgType = 0;
            std::memcpy(&msgType, &msg.header.messageType, sizeof(msgType));
            msgType = _byteswap_ushort(msgType);

            if (msgType == static_cast<uint16_t>(MessageType::Ping)) {
                uint64_t tid = 0;
                std::memcpy(&tid, &msg.header.taskId, sizeof(tid));
                auto pong = Message::makePong(_byteswap_uint64(tid));
                conn.sendMessage(pong);
                continue;
            }

            bool isCompressed = (msgType == static_cast<uint16_t>(MessageType::SubmitTaskCompressed)) ||
                                hasHeaderFlag(msg.header, FLAG_COMPRESSED);

            TaskRequest req;
            if (isCompressed) {
                uint32_t uncompSize = 0;
                std::memcpy(&uncompSize, msg.payload.data(), 4);
                auto decomp = decompressBuffer(
                    std::span<const std::byte>(msg.payload.data() + 4, msg.payload.size() - 4),
                    uncompSize);
                req = TaskRequest::deserialise(decomp);
            } else {
                req = TaskRequest::deserialise(msg.payload);
            }

            TaskResult res;
            res.taskId = req.taskId;
            res.status = TaskStatus::Ok;
            res.output = req.input; // Echo back same buffer

            Message resp;
            auto pl = res.serialise();
            if (isCompressed) {
                pl = compressBuffer(pl);
                resp.header.messageType = _byteswap_ushort(static_cast<uint16_t>(MessageType::TaskResultCompressed));
                setHeaderFlag(resp.header, FLAG_COMPRESSED);
            } else {
                resp.header.messageType = _byteswap_ushort(static_cast<uint16_t>(MessageType::TaskResult));
            }
            resp.header.magic       = _byteswap_ulong(MAGIC);
            resp.header.version     = _byteswap_ushort(PROTO_VERSION);
            resp.header.taskId      = _byteswap_uint64(req.taskId);
            resp.header.payloadSize = _byteswap_ulong(static_cast<uint32_t>(pl.size()));
            resp.payload            = std::move(pl);
            conn.sendMessage(resp);
        }
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TaskClient client;
    CHECK(client.connect("127.0.0.1", 17784));

    // Case 1: Compressible payload + 10 Mbps bandwidth -> should compress
    {
        client.setLinkBandwidthBps(10'000'000); // 10 Mbps
        TaskRequest req;
        req.taskId = 501;
        req.type   = TaskType::Blur;
        req.width  = 128;
        req.height = 128;
        req.input.resize(128 * 128 * 4, static_cast<std::byte>(0x3C));

        auto res = client.submit(req);
        CHECK(res.status == TaskStatus::Ok);
        CHECK(res.taskId == 501);

        auto tm = client.lastTransferMetrics();
        CHECK(tm.wasCompressed);
        CHECK(tm.compressedSendBytes < tm.uncompressedSendBytes);
        CHECK(tm.compressionRatioSend > 2.0f);
    }

    // Case 2: High-entropy payload + 10 Gbps bandwidth -> should bypass compression
    {
        client.setLinkBandwidthBps(10'000'000'000ull); // 10 Gbps
        TaskRequest req;
        req.taskId = 502;
        req.type   = TaskType::Blur;
        req.width  = 128;
        req.height = 128;
        req.input.resize(128 * 128 * 4);
        uint32_t state = 987654321u;
        for (size_t i = 0; i < req.input.size(); ++i) {
            state = state * 1664525u + 1013904223u;
            req.input[i] = static_cast<std::byte>((state >> 16) & 0xFF);
        }

        auto res = client.submit(req);
        CHECK(res.status == TaskStatus::Ok);
        CHECK(res.taskId == 502);

        auto tm = client.lastTransferMetrics();
        CHECK(!tm.wasCompressed);
        CHECK(tm.compressedSendBytes == tm.uncompressedSendBytes);
        CHECK(tm.compressionRatioSend == 1.0f);
    }

    // Verify stats tracked both compress and bypass decisions
    auto stats = client.transferPolicy().stats();
    CHECK(stats.compressDecisions >= 1);
    CHECK(stats.bypassDecisions >= 1);
    CHECK(stats.totalBytesEvaluated > 0);

    client.disconnect();
    server.stop();
    std::cout << "  adaptive transfer policy client: PASS\n";
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Networking Tests ===\n";
    test_ping_pong();
    test_bad_magic();
    test_oversized_payload();
    test_truncated_stream();
    test_large_payload();
    test_worker_capabilities_negotiation();
    test_health_monitor_state_transitions();
    test_adaptive_transfer_policy_client();
    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
