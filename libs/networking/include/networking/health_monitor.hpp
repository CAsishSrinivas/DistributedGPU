// networking/health_monitor.hpp
// Configurable worker health monitoring and automated fail-safe recovery.
#pragma once

#include "networking/tcp_connection.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace dgpu::networking {

// ─── Health states ────────────────────────────────────────────────────────────

enum class WorkerHealthState {
    Healthy,
    Suspect,
    Disconnected,
    Recovering
};

inline const char* healthStateToString(WorkerHealthState state) noexcept {
    switch (state) {
    case WorkerHealthState::Healthy:      return "Healthy";
    case WorkerHealthState::Suspect:      return "Suspect";
    case WorkerHealthState::Disconnected: return "Disconnected";
    case WorkerHealthState::Recovering:   return "Recovering";
    }
    return "Unknown";
}

// ─── Health monitor configuration ─────────────────────────────────────────────

struct HealthMonitorConfig {
    uint32_t heartbeatIntervalMs{1000};
    uint32_t heartbeatTimeoutMs{3000};
    uint32_t suspectThreshold{1};      // Consecutive failures to enter Suspect
    uint32_t disconnectThreshold{2};   // Consecutive failures to enter Disconnected
    uint32_t initialBackoffMs{200};
    uint32_t maxBackoffMs{5000};
    float    backoffMultiplier{2.0f};
    bool     autoReconnect{true};
};

using StateChangeCallback = std::function<void(WorkerHealthState oldState, WorkerHealthState newState)>;

// ─── WorkerHealthMonitor ──────────────────────────────────────────────────────

class WorkerHealthMonitor {
public:
    WorkerHealthMonitor(TaskClient& client,
                        std::string workerHost,
                        uint16_t    workerPort,
                        HealthMonitorConfig config = {});
    ~WorkerHealthMonitor();

    WorkerHealthMonitor(const WorkerHealthMonitor&)            = delete;
    WorkerHealthMonitor& operator=(const WorkerHealthMonitor&) = delete;

    void start();
    void stop();

    [[nodiscard]] WorkerHealthState state() const noexcept;
    [[nodiscard]] const char* stateName() const noexcept;
    [[nodiscard]] bool isHealthy() const noexcept;

    // Metrics counters
    [[nodiscard]] uint64_t successfulHeartbeats() const noexcept;
    [[nodiscard]] uint64_t failedHeartbeats() const noexcept;
    [[nodiscard]] uint64_t reconnectAttempts() const noexcept;
    [[nodiscard]] uint64_t stateTransitionCount() const noexcept;

    void setStateCallback(StateChangeCallback cb);

    // Forces an immediate health check (synchronous probe)
    [[nodiscard]] bool probeNow();

private:
    void monitorLoop();
    void transitionTo(WorkerHealthState newState);

    TaskClient&                    m_client;
    std::string                    m_host;
    uint16_t                       m_port;
    HealthMonitorConfig            m_config;

    std::atomic<WorkerHealthState> m_state{WorkerHealthState::Disconnected};
    StateChangeCallback            m_callback;
    mutable std::mutex             m_cbMu;

    std::atomic_bool               m_running{false};
    std::thread                    m_thread;

    std::atomic<uint64_t>          m_successHeartbeats{0};
    std::atomic<uint64_t>          m_failedHeartbeats{0};
    std::atomic<uint64_t>          m_reconnectAttempts{0};
    std::atomic<uint64_t>          m_stateTransitions{0};

    std::condition_variable        m_cv;
    std::mutex                     m_cvMu;
};

} // namespace dgpu::networking
