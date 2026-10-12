// networking/tcp_connection.hpp
// Wraps a Winsock TCP socket with framed message I/O.
#pragma once

#include "protocol/message.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace dgpu::networking {

// ─── Connection options ───────────────────────────────────────────────────────

struct ConnectionOptions {
    uint32_t connectTimeoutMs  = 10'000;
    uint32_t sendTimeoutMs     = 30'000;
    uint32_t recvTimeoutMs     = 60'000;
    uint32_t keepAliveIdleMs   = 15'000;
    uint32_t maxPayloadBytes   = protocol::MAX_PAYLOAD;
};

// ─── TcpConnection ───────────────────────────────────────────────────────────
// Wraps a single connected TCP socket.
// Thread-safety: send and receive may be called from separate threads,
//                but concurrent sends or concurrent receives are not safe.

class TcpConnection {
public:
    TcpConnection();
    explicit TcpConnection(uintptr_t existingSocket, const ConnectionOptions& opts = {});
    ~TcpConnection();

    // Non-copyable, movable
    TcpConnection(const TcpConnection&)            = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&)                 noexcept;
    TcpConnection& operator=(TcpConnection&&)      noexcept;

    // Connect to a remote host (client side)
    [[nodiscard]] bool connect(const std::string& host,
                               uint16_t           port,
                               const ConnectionOptions& opts = {});

    // Send a complete framed message (header + payload)
    [[nodiscard]] bool sendMessage(const protocol::Message& msg);

    // Receive a complete framed message; blocks until complete or error
    [[nodiscard]] bool receiveMessage(protocol::Message& msg);

    // Non-blocking check — true if the socket appears usable
    [[nodiscard]] bool isConnected() const noexcept;

    void close();

    // Peer address for logging
    [[nodiscard]] std::string peerAddress() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ─── TaskServer ──────────────────────────────────────────────────────────────
// Accepts incoming connections and dispatches them to a handler.

using ConnectionHandler = std::function<void(TcpConnection)>;

class TaskServer {
public:
    TaskServer();
    ~TaskServer();

    TaskServer(const TaskServer&)            = delete;
    TaskServer& operator=(const TaskServer&) = delete;

    // Bind and listen; handler is called on a new thread for each connection
    [[nodiscard]] bool start(uint16_t port, ConnectionHandler handler);

    // Signal the accept loop to stop (existing handler threads complete normally)
    void stop();

    [[nodiscard]] bool isRunning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ─── TaskClient ──────────────────────────────────────────────────────────────
// Persistent single-connection client with submit/receive pairing.

class TaskClient {
public:
    TaskClient();
    ~TaskClient();

    TaskClient(const TaskClient&)            = delete;
    TaskClient& operator=(const TaskClient&) = delete;

    [[nodiscard]] bool connect(const std::string& host,
                               uint16_t           port,
                               const ConnectionOptions& opts = {});

    [[nodiscard]] protocol::TaskResult submit(const protocol::TaskRequest& req);

    [[nodiscard]] bool ping();

    void disconnect();

    [[nodiscard]] bool isConnected() const noexcept;

    // LZ4 Payload Compression
    void setCompressionEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isCompressionEnabled() const noexcept;

    struct TransferMetrics {
        size_t   uncompressedSendBytes{0};
        size_t   compressedSendBytes{0};
        uint64_t compressionUs{0};
        size_t   compressedRecvBytes{0};
        size_t   uncompressedRecvBytes{0};
        uint64_t decompressionUs{0};
        float    compressionRatioSend{1.0f};
        float    compressionRatioRecv{1.0f};
    };

    [[nodiscard]] TransferMetrics lastTransferMetrics() const noexcept;

private:
    TcpConnection       m_conn;
    uint64_t            m_nextTaskId{1};
    bool                m_compressionEnabled{true};
    TransferMetrics     m_lastMetrics;
    mutable std::mutex  m_submitMu;
};

// ─── Winsock RAII guard ───────────────────────────────────────────────────────
// Construct once at the beginning of main() on the host and worker.

class WinsockInit {
public:
    WinsockInit();
    ~WinsockInit();
    [[nodiscard]] bool ok() const noexcept { return m_ok; }

private:
    bool m_ok{false};
};

} // namespace dgpu::networking
