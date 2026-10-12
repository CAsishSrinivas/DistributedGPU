// networking/tcp_connection.cpp
// Winsock TCP implementation with framed message I/O.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include "networking/tcp_connection.hpp"
#include "protocol/lz4.hpp"

#include <array>
#include <cstring>
#include <format>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dgpu::networking {

// ─── WinsockInit ─────────────────────────────────────────────────────────────

WinsockInit::WinsockInit() {
    WSADATA wd{};
    m_ok = (WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    if (!m_ok) {
        std::cerr << "[networking] WSAStartup failed: " << WSAGetLastError() << '\n';
    }
}
WinsockInit::~WinsockInit() { if (m_ok) WSACleanup(); }

// ─── TcpConnection::Impl ─────────────────────────────────────────────────────

struct TcpConnection::Impl {
    SOCKET          sock{INVALID_SOCKET};
    ConnectionOptions opts{};
    std::string     peer;

    bool sendAll(const void* data, size_t size) {
        const auto* p = static_cast<const char*>(data);
        size_t sent   = 0;
        while (sent < size) {
            int n = ::send(sock, p + sent,
                           static_cast<int>(size - sent), 0);
            if (n <= 0) {
                std::cerr << "[tcp] send error (n=" << n << "): " << WSAGetLastError() << '\n';
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool recvAll(void* data, size_t size) {
        auto* p = static_cast<char*>(data);
        size_t recvd = 0;
        while (recvd < size) {
            int n = ::recv(sock, p + recvd,
                           static_cast<int>(size - recvd), 0);
            if (n <= 0) {
                if (n == 0) std::cerr << "[tcp] peer disconnected\n";
                else        std::cerr << "[tcp] recv error: " << WSAGetLastError() << '\n';
                return false;
            }
            recvd += static_cast<size_t>(n);
        }
        return true;
    }

    void applyTimeouts() {
        DWORD to = opts.sendTimeoutMs;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&to), sizeof(to));
        to = opts.recvTimeoutMs;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&to), sizeof(to));
        // TCP keepalive
        BOOL ka = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char*>(&ka), sizeof(ka));
    }
};

// ─── TcpConnection ───────────────────────────────────────────────────────────

TcpConnection::TcpConnection() : m_impl(std::make_unique<Impl>()) {}

TcpConnection::TcpConnection(uintptr_t existingSocket, const ConnectionOptions& opts)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->sock = static_cast<SOCKET>(existingSocket);
    m_impl->opts = opts;
    m_impl->applyTimeouts();
}

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&&) noexcept = default;
TcpConnection& TcpConnection::operator=(TcpConnection&&) noexcept = default;

bool TcpConnection::connect(const std::string& host, uint16_t port,
                             const ConnectionOptions& opts) {
    static WinsockInit s_autoWinsock;
    m_impl->opts = opts;

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    auto portStr = std::to_string(port);
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        std::cerr << "[tcp] getaddrinfo failed: " << WSAGetLastError() << '\n';
        return false;
    }

    SOCKET s = INVALID_SOCKET;
    for (auto* r = res; r != nullptr; r = r->ai_next) {
        s = ::socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, r->ai_addr, static_cast<int>(r->ai_addrlen)) == 0) break;
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        std::cerr << "[tcp] connect to " << host << ':' << port << " failed\n";
        return false;
    }

    m_impl->sock = s;
    m_impl->peer = host + ':' + portStr;
    m_impl->applyTimeouts();
    return true;
}

bool TcpConnection::sendMessage(const protocol::Message& msg) {
    // 1. Send header
    protocol::MessageHeader hdr = msg.header;
    if (!m_impl->sendAll(&hdr, sizeof(hdr))) return false;

    // 2. Send payload (may be empty for control messages)
    if (!msg.payload.empty()) {
        if (!m_impl->sendAll(msg.payload.data(), msg.payload.size())) return false;
    }
    return true;
}

bool TcpConnection::receiveMessage(protocol::Message& out) {
    protocol::MessageHeader hdr{};
    if (!m_impl->recvAll(&hdr, sizeof(hdr))) return false;

    // Validate magic (network byte order comparison)
    uint32_t magic = 0;
    std::memcpy(&magic, &hdr.magic, sizeof(magic));
#if defined(_WIN32)
    magic = _byteswap_ulong(magic);
#else
    magic = __builtin_bswap32(magic);
#endif
    if (magic != protocol::MAGIC) {
        std::cerr << "[tcp] bad magic 0x" << std::hex << magic << '\n';
        return false;
    }

    // Validate protocol version
    uint16_t version = 0;
    std::memcpy(&version, &hdr.version, sizeof(version));
#if defined(_WIN32)
    version = _byteswap_ushort(version);
#else
    version = __builtin_bswap16(version);
#endif
    if (version != protocol::PROTO_VERSION) {
        std::cerr << "[tcp] unsupported protocol version: " << version << '\n';
        return false;
    }

    // Validate message type
    uint16_t msgType = 0;
    std::memcpy(&msgType, &hdr.messageType, sizeof(msgType));
#if defined(_WIN32)
    msgType = _byteswap_ushort(msgType);
#else
    msgType = __builtin_bswap16(msgType);
#endif
    switch (static_cast<protocol::MessageType>(msgType)) {
    case protocol::MessageType::Hello:
    case protocol::MessageType::HelloAck:
    case protocol::MessageType::SubmitTask:
    case protocol::MessageType::SubmitTaskCompressed:
    case protocol::MessageType::TaskAccepted:
    case protocol::MessageType::TaskResult:
    case protocol::MessageType::TaskResultCompressed:
    case protocol::MessageType::TaskError:
    case protocol::MessageType::Ping:
    case protocol::MessageType::Pong:
    case protocol::MessageType::Shutdown:
        break;
    default:
        std::cerr << "[tcp] unknown message type 0x" << std::hex << msgType << '\n';
        return false;
    }

    // Decode payload size
    uint32_t payloadSize = 0;
    std::memcpy(&payloadSize, &hdr.payloadSize, sizeof(payloadSize));
#if defined(_WIN32)
    payloadSize = _byteswap_ulong(payloadSize);
#else
    payloadSize = __builtin_bswap32(payloadSize);
#endif

    if (payloadSize > m_impl->opts.maxPayloadBytes) {
        std::cerr << "[tcp] payload too large: " << payloadSize << " bytes\n";
        return false;
    }

    out.header  = hdr;
    out.payload.resize(payloadSize);
    if (payloadSize > 0) {
        if (!m_impl->recvAll(out.payload.data(), payloadSize)) return false;
    }
    return true;
}

bool TcpConnection::isConnected() const noexcept {
    return m_impl && m_impl->sock != INVALID_SOCKET;
}

void TcpConnection::close() {
    if (m_impl && m_impl->sock != INVALID_SOCKET) {
        ::shutdown(m_impl->sock, SD_BOTH);
        ::closesocket(m_impl->sock);
        m_impl->sock = INVALID_SOCKET;
    }
}

std::string TcpConnection::peerAddress() const {
    return m_impl ? m_impl->peer : "(none)";
}

// ─── TaskServer::Impl ────────────────────────────────────────────────────────

struct TaskServer::Impl {
    SOCKET                   listenSock{INVALID_SOCKET};
    std::atomic_bool         running{false};
    std::thread              acceptThread;
    ConnectionHandler        handler;
    std::vector<std::thread> clientThreads;
    std::vector<SOCKET>      activeClientSockets;
    std::mutex               threadsMu;
};

TaskServer::TaskServer() : m_impl(std::make_unique<Impl>()) {}
TaskServer::~TaskServer() { stop(); }

bool TaskServer::start(uint16_t port, ConnectionHandler handler) {
    static WinsockInit s_autoWinsock;
    m_impl->handler = std::move(handler);

    m_impl->listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->listenSock == INVALID_SOCKET) return false;

    BOOL reuseAddr = TRUE;
    setsockopt(m_impl->listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(m_impl->listenSock, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[server] bind failed: " << WSAGetLastError() << '\n';
        ::closesocket(m_impl->listenSock);
        m_impl->listenSock = INVALID_SOCKET;
        return false;
    }

    if (::listen(m_impl->listenSock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[server] listen failed: " << WSAGetLastError() << '\n';
        ::closesocket(m_impl->listenSock);
        m_impl->listenSock = INVALID_SOCKET;
        return false;
    }

    m_impl->running = true;
    m_impl->acceptThread = std::thread([this] {
        while (m_impl->running) {
            SOCKET client = ::accept(m_impl->listenSock, nullptr, nullptr);
            if (client == INVALID_SOCKET) break; // stop() closed the listen socket
            
            std::lock_guard<std::mutex> lk(m_impl->threadsMu);
            m_impl->activeClientSockets.push_back(client);
            m_impl->clientThreads.emplace_back([this, h = m_impl->handler, client]() {
                ConnectionOptions sOpts;
                sOpts.recvTimeoutMs = 0; // Infinite on server: wait for game frames without timing out
                sOpts.sendTimeoutMs = 30'000;
                TcpConnection conn(static_cast<uintptr_t>(client), sOpts);
                h(std::move(conn));

                std::lock_guard<std::mutex> lk2(m_impl->threadsMu);
                auto it = std::find(m_impl->activeClientSockets.begin(),
                                    m_impl->activeClientSockets.end(), client);
                if (it != m_impl->activeClientSockets.end()) {
                    m_impl->activeClientSockets.erase(it);
                }
            });
        }
    });

    std::cout << "[server] listening on port " << port << '\n';
    return true;
}

void TaskServer::stop() {
    m_impl->running = false;
    if (m_impl->listenSock != INVALID_SOCKET) {
        ::closesocket(m_impl->listenSock);
        m_impl->listenSock = INVALID_SOCKET;
    }

    // Signal shutdown on active client sockets to unblock any pending recv() operations.
    // NOTE: We only call ::shutdown(s, SD_BOTH) here; the actual handle closing is left to
    // TcpConnection's destructor so the socket handle is owned and closed safely exactly once.
    {
        std::lock_guard<std::mutex> lk(m_impl->threadsMu);
        for (SOCKET s : m_impl->activeClientSockets) {
            ::shutdown(s, SD_BOTH);
            CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
        }
    }

    if (m_impl->acceptThread.joinable()) m_impl->acceptThread.join();

    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lk(m_impl->threadsMu);
        toJoin = std::move(m_impl->clientThreads);
    }

    // Join threads OUTSIDE of mutex lock so finishing threads can acquire threadsMu without deadlock
    for (auto& t : toJoin) {
        if (t.joinable()) t.join();
    }
}

bool TaskServer::isRunning() const noexcept { return m_impl->running; }

// ─── TaskClient ──────────────────────────────────────────────────────────────

TaskClient::TaskClient() = default;
TaskClient::~TaskClient() { disconnect(); }

bool TaskClient::connect(const std::string& host, uint16_t port,
                          const ConnectionOptions& opts) {
    std::lock_guard<std::mutex> lk(m_submitMu);
    if (!m_conn.connect(host, port, opts)) return false;

    // Handshake: send HELLO and await valid HELLO_ACK
    auto hello = protocol::Message::makeHello("HostA", "RTX 3050", 4096);
    if (!m_conn.sendMessage(hello)) {
        m_conn.close();
        return false;
    }

    protocol::Message ack;
    if (!m_conn.receiveMessage(ack)) {
        m_conn.close();
        return false;
    }

    uint32_t magic   = _byteswap_ulong(ack.header.magic);
    uint16_t version = _byteswap_ushort(ack.header.version);
    uint16_t type    = _byteswap_ushort(ack.header.messageType);

    if (magic != protocol::MAGIC || version != protocol::PROTO_VERSION ||
        type != static_cast<uint16_t>(protocol::MessageType::HelloAck)) {
        m_conn.close();
        return false;
    }

    return true;
}

protocol::TaskResult TaskClient::submit(const protocol::TaskRequest& req) {
    std::lock_guard<std::mutex> lk(m_submitMu);

    // Build SUBMIT_TASK message
    auto payload = req.serialise();
    protocol::Message msg;
    msg.header.magic       = _byteswap_ulong(protocol::MAGIC);
    msg.header.version     = _byteswap_ushort(protocol::PROTO_VERSION);
    msg.header.taskId      = _byteswap_uint64(req.taskId);
    msg.header.status      = 0;

    TransferMetrics tm;
    tm.uncompressedSendBytes = payload.size();

    if (m_compressionEnabled) {
        msg.header.messageType = _byteswap_ushort(
            static_cast<uint16_t>(protocol::MessageType::SubmitTaskCompressed));
        auto c0 = std::chrono::steady_clock::now();
        msg.payload = protocol::compressBuffer(payload);
        auto c1 = std::chrono::steady_clock::now();
        tm.compressionUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(c1 - c0).count());
        tm.compressedSendBytes = msg.payload.size();
        tm.compressionRatioSend = (tm.compressedSendBytes > 0)
                                      ? static_cast<float>(tm.uncompressedSendBytes) / static_cast<float>(tm.compressedSendBytes)
                                      : 1.0f;
    } else {
        msg.header.messageType = _byteswap_ushort(
            static_cast<uint16_t>(protocol::MessageType::SubmitTask));
        msg.payload = std::move(payload);
        tm.compressedSendBytes = tm.uncompressedSendBytes;
        tm.compressionRatioSend = 1.0f;
    }
    msg.header.payloadSize = _byteswap_ulong(static_cast<uint32_t>(msg.payload.size()));

    if (!m_conn.sendMessage(msg)) {
        m_conn.close();
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    // Wait for response
    protocol::Message resp;
    if (!m_conn.receiveMessage(resp)) {
        m_conn.close();
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    uint32_t magic      = _byteswap_ulong(resp.header.magic);
    uint16_t version    = _byteswap_ushort(resp.header.version);
    uint16_t rawType    = _byteswap_ushort(resp.header.messageType);
    uint64_t respTaskId = _byteswap_uint64(resp.header.taskId);
    uint32_t respStatus = _byteswap_ulong(resp.header.status);

    if (magic != protocol::MAGIC || version != protocol::PROTO_VERSION) {
        std::cerr << "[task_client] Invalid response header magic/version\n";
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    auto msgType = static_cast<protocol::MessageType>(rawType);
    if (msgType != protocol::MessageType::TaskResult &&
        msgType != protocol::MessageType::TaskResultCompressed &&
        msgType != protocol::MessageType::TaskError) {
        std::cerr << "[task_client] Unexpected response message type 0x" << std::hex << rawType << std::dec << '\n';
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    if (respTaskId != req.taskId) {
        std::cerr << "[task_client] Task ID mismatch: expected " << req.taskId << " got " << respTaskId << '\n';
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    if (msgType == protocol::MessageType::TaskError || respStatus != 0) {
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = (respStatus != 0) ? static_cast<protocol::TaskStatus>(respStatus)
                                       : protocol::TaskStatus::InternalError;
        return err;
    }

    try {
        std::span<const std::byte> payloadSpan = resp.payload;
        std::vector<std::byte> decomp;

        if (msgType == protocol::MessageType::TaskResultCompressed) {
            if (resp.payload.size() < 4) {
                throw std::runtime_error("Compressed task result payload too short");
            }
            tm.compressedRecvBytes = resp.payload.size();
            uint32_t uncompSize = 0;
            std::memcpy(&uncompSize, resp.payload.data(), 4);
            tm.uncompressedRecvBytes = uncompSize;
            auto d0 = std::chrono::steady_clock::now();
            decomp = protocol::decompressBuffer(
                std::span<const std::byte>(resp.payload.data() + 4, resp.payload.size() - 4),
                uncompSize);
            auto d1 = std::chrono::steady_clock::now();
            tm.decompressionUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count());
            tm.compressionRatioRecv = (tm.compressedRecvBytes > 0)
                                          ? static_cast<float>(tm.uncompressedRecvBytes) / static_cast<float>(tm.compressedRecvBytes)
                                          : 1.0f;
            payloadSpan = decomp;
        } else {
            tm.compressedRecvBytes = resp.payload.size();
            tm.uncompressedRecvBytes = resp.payload.size();
            tm.compressionRatioRecv = 1.0f;
        }
        m_lastMetrics = tm;

        auto res = protocol::TaskResult::deserialise(payloadSpan);
        if (res.taskId != req.taskId) {
            res.status = protocol::TaskStatus::InternalError;
        }
        return res;
    } catch (const std::exception& e) {
        std::cerr << "[task_client] Failed to deserialise task result: " << e.what() << '\n';
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }
}

bool TaskClient::ping() {
    std::lock_guard<std::mutex> lk(m_submitMu);
    uint64_t pingId = m_nextTaskId++;
    auto ping = protocol::Message::makePing(pingId);
    if (!m_conn.sendMessage(ping)) return false;
    protocol::Message pong;
    if (!m_conn.receiveMessage(pong)) return false;

    uint32_t magic   = _byteswap_ulong(pong.header.magic);
    uint16_t version = _byteswap_ushort(pong.header.version);
    uint16_t type    = _byteswap_ushort(pong.header.messageType);
    uint64_t taskId  = _byteswap_uint64(pong.header.taskId);

    return (magic == protocol::MAGIC &&
            version == protocol::PROTO_VERSION &&
            type == static_cast<uint16_t>(protocol::MessageType::Pong) &&
            taskId == pingId);
}

void TaskClient::disconnect() {
    std::lock_guard<std::mutex> lk(m_submitMu);
    m_conn.close();
}

bool TaskClient::isConnected() const noexcept {
    std::lock_guard<std::mutex> lk(m_submitMu);
    return m_conn.isConnected();
}

void TaskClient::setCompressionEnabled(bool enabled) noexcept {
    std::lock_guard<std::mutex> lk(m_submitMu);
    m_compressionEnabled = enabled;
}

bool TaskClient::isCompressionEnabled() const noexcept {
    std::lock_guard<std::mutex> lk(m_submitMu);
    return m_compressionEnabled;
}

TaskClient::TransferMetrics TaskClient::lastTransferMetrics() const noexcept {
    std::lock_guard<std::mutex> lk(m_submitMu);
    return m_lastMetrics;
}

} // namespace dgpu::networking

