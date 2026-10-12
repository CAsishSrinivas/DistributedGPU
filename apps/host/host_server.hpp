// apps/host/host_server.hpp
// Interface for the host-side worker-connection manager.
#pragma once

#include "networking/tcp_connection.hpp"
#include "protocol/message.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace dgpu::host {

struct ServerConfig {
    uint32_t pingIntervalMs  = 5000;  // Health-check ping interval
    uint32_t maxWorkers      = 1;     // Only one worker supported in v1
};

// ─── WorkerProxy ─────────────────────────────────────────────────────────────
// Represents a single connected worker from the host's perspective.
// Thread-safe: a single mutex serialises all network operations.

class WorkerProxy {
public:
    explicit WorkerProxy(networking::TcpConnection conn);
    ~WorkerProxy();

    WorkerProxy(const WorkerProxy&)            = delete;
    WorkerProxy& operator=(const WorkerProxy&) = delete;

    [[nodiscard]] protocol::TaskResult submit(const protocol::TaskRequest& req);
    [[nodiscard]] bool ping();
    void disconnect();
    [[nodiscard]] bool isConnected() const noexcept;

private:
    networking::TcpConnection m_conn;
    std::atomic_bool          m_connected;
    mutable std::mutex        m_mu;
};

// ─── HostServer ──────────────────────────────────────────────────────────────
// Accepts worker connections and exposes the active WorkerProxy.

class HostServer {
public:
    HostServer();
    ~HostServer();

    HostServer(const HostServer&)            = delete;
    HostServer& operator=(const HostServer&) = delete;

    [[nodiscard]] bool start(uint16_t port, const ServerConfig& cfg = {});
    void stop();

    [[nodiscard]] std::shared_ptr<WorkerProxy> workerProxy() const;
    [[nodiscard]] bool hasWorker() const noexcept;

private:
    void onWorkerConnected(networking::TcpConnection conn);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::host
