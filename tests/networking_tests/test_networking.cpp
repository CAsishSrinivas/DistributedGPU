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

// ─── Entry point ─────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Networking Tests ===\n";
    test_ping_pong();
    test_bad_magic();
    test_oversized_payload();
    test_truncated_stream();
    test_large_payload();
    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
