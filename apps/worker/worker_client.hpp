// apps/worker/worker_client.hpp
// Manages the persistent outbound connection from the worker to the host
// (optional reverse channel for worker-initiated status reports).
// In v1 the worker is purely reactive; this class provides the foundation
// for M5 worker-push heartbeats and for future bidirectional scheduling.
#pragma once

#include "networking/tcp_connection.hpp"
#include "protocol/message.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace dgpu::worker {

// ─── WorkerStatus — reported to the host ─────────────────────────────────────

struct WorkerStatus {
    std::string gpuName;
    uint32_t    vramMb{};
    uint32_t    pendingTasks{};
    double      gpuUtilisationPct{};   // 0–100
    uint64_t    totalTasksCompleted{};
    uint64_t    totalGpuUs{};
};

// ─── WorkerClient ────────────────────────────────────────────────────────────
// Maintains a persistent connection back to the host for heartbeats.
// Runs a background thread that sends periodic PING messages; if the host
// does not respond, the connection is marked dead.

class WorkerClient {
public:
    WorkerClient();
    ~WorkerClient();

    WorkerClient(const WorkerClient&)            = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    // Connect to the host (optional — only needed for reverse-channel features)
    [[nodiscard]] bool connect(const std::string& hostAddress,
                               uint16_t           port,
                               uint32_t           timeoutMs = 5000);

    // Start the background heartbeat thread
    void startHeartbeat(std::chrono::milliseconds interval = std::chrono::seconds{10});

    // Stop and disconnect
    void stop();

    // Update the status that the next heartbeat will report
    void updateStatus(const WorkerStatus& status);

    [[nodiscard]] bool isConnected() const noexcept;

private:
    void heartbeatLoop(std::chrono::milliseconds interval);

    networking::TcpConnection m_conn;
    std::thread               m_heartbeatThread;
    std::atomic_bool          m_running{false};
    WorkerStatus              m_status;
    mutable std::mutex        m_statusMu;
};

} // namespace dgpu::worker
