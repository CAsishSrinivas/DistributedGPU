// networking/health_monitor.cpp
// Automated worker health monitoring, state transitions, and bounded exponential backoff reconnection.

#include "networking/health_monitor.hpp"

#include <algorithm>
#include <iostream>

namespace dgpu::networking {

WorkerHealthMonitor::WorkerHealthMonitor(TaskClient& client,
                                         std::string workerHost,
                                         uint16_t    workerPort,
                                         HealthMonitorConfig config)
    : m_client(client)
    , m_host(std::move(workerHost))
    , m_port(workerPort)
    , m_config(config) {
    if (m_client.isConnected()) {
        m_state.store(WorkerHealthState::Healthy);
    } else {
        m_state.store(WorkerHealthState::Disconnected);
    }
}

WorkerHealthMonitor::~WorkerHealthMonitor() {
    stop();
}

void WorkerHealthMonitor::start() {
    if (m_running.exchange(true)) return; // Already running

    if (m_client.isConnected()) {
        m_state.store(WorkerHealthState::Healthy);
    } else {
        m_state.store(WorkerHealthState::Disconnected);
    }

    m_thread = std::thread(&WorkerHealthMonitor::monitorLoop, this);
}

void WorkerHealthMonitor::stop() {
    if (!m_running.exchange(false)) return;

    {
        std::lock_guard<std::mutex> lk(m_cvMu);
        m_cv.notify_all();
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

WorkerHealthState WorkerHealthMonitor::state() const noexcept {
    return m_state.load();
}

const char* WorkerHealthMonitor::stateName() const noexcept {
    return healthStateToString(m_state.load());
}

bool WorkerHealthMonitor::isHealthy() const noexcept {
    return m_state.load() == WorkerHealthState::Healthy;
}

uint64_t WorkerHealthMonitor::successfulHeartbeats() const noexcept {
    return m_successHeartbeats.load();
}

uint64_t WorkerHealthMonitor::failedHeartbeats() const noexcept {
    return m_failedHeartbeats.load();
}

uint64_t WorkerHealthMonitor::reconnectAttempts() const noexcept {
    return m_reconnectAttempts.load();
}

uint64_t WorkerHealthMonitor::stateTransitionCount() const noexcept {
    return m_stateTransitions.load();
}

void WorkerHealthMonitor::setStateCallback(StateChangeCallback cb) {
    std::lock_guard<std::mutex> lk(m_cbMu);
    m_callback = std::move(cb);
}

void WorkerHealthMonitor::transitionTo(WorkerHealthState newState) {
    WorkerHealthState oldState = m_state.exchange(newState);
    if (oldState != newState) {
        ++m_stateTransitions;
        StateChangeCallback cbCopy;
        {
            std::lock_guard<std::mutex> lk(m_cbMu);
            cbCopy = m_callback;
        }
        if (cbCopy) {
            try {
                cbCopy(oldState, newState);
            } catch (...) {
                // Suppress exception from user callback to maintain monitor loop stability
            }
        }
    }
}

bool WorkerHealthMonitor::probeNow() {
    if (!m_client.isConnected()) {
        transitionTo(WorkerHealthState::Disconnected);
        return false;
    }

    bool ok = m_client.ping();
    if (ok) {
        ++m_successHeartbeats;
        transitionTo(WorkerHealthState::Healthy);
        return true;
    } else {
        ++m_failedHeartbeats;
        if (m_state.load() == WorkerHealthState::Healthy) {
            transitionTo(WorkerHealthState::Suspect);
        } else {
            m_client.disconnect();
            transitionTo(WorkerHealthState::Disconnected);
        }
        return false;
    }
}

void WorkerHealthMonitor::monitorLoop() {
    uint32_t consecutiveFailures = 0;
    uint32_t currentBackoffMs    = m_config.initialBackoffMs;

    while (m_running.load()) {
        WorkerHealthState cur = m_state.load();

        switch (cur) {
        case WorkerHealthState::Healthy: {
            // Wait for heartbeat interval
            {
                std::unique_lock<std::mutex> lk(m_cvMu);
                m_cv.wait_for(lk, std::chrono::milliseconds(m_config.heartbeatIntervalMs),
                              [this] { return !m_running.load(); });
            }
            if (!m_running.load()) break;

            if (!m_client.isConnected()) {
                consecutiveFailures = m_config.disconnectThreshold;
                transitionTo(WorkerHealthState::Disconnected);
                break;
            }

            if (m_client.ping()) {
                ++m_successHeartbeats;
                consecutiveFailures = 0;
            } else {
                ++m_failedHeartbeats;
                ++consecutiveFailures;
                if (consecutiveFailures >= m_config.disconnectThreshold) {
                    m_client.disconnect();
                    transitionTo(WorkerHealthState::Disconnected);
                } else if (consecutiveFailures >= m_config.suspectThreshold) {
                    transitionTo(WorkerHealthState::Suspect);
                }
            }
            break;
        }

        case WorkerHealthState::Suspect: {
            // In suspect state, probe rapidly (half interval or 200ms)
            uint32_t probeWaitMs = std::max(50u, m_config.heartbeatIntervalMs / 2);
            {
                std::unique_lock<std::mutex> lk(m_cvMu);
                m_cv.wait_for(lk, std::chrono::milliseconds(probeWaitMs),
                              [this] { return !m_running.load(); });
            }
            if (!m_running.load()) break;

            if (m_client.isConnected() && m_client.ping()) {
                ++m_successHeartbeats;
                consecutiveFailures = 0;
                transitionTo(WorkerHealthState::Healthy);
            } else {
                ++m_failedHeartbeats;
                ++consecutiveFailures;
                m_client.disconnect();
                transitionTo(WorkerHealthState::Disconnected);
            }
            break;
        }

        case WorkerHealthState::Disconnected: {
            consecutiveFailures = 0;
            currentBackoffMs    = m_config.initialBackoffMs;
            if (m_config.autoReconnect) {
                transitionTo(WorkerHealthState::Recovering);
            } else {
                std::unique_lock<std::mutex> lk(m_cvMu);
                m_cv.wait(lk, [this] { return !m_running.load(); });
            }
            break;
        }

        case WorkerHealthState::Recovering: {
            // Bounded exponential backoff wait
            {
                std::unique_lock<std::mutex> lk(m_cvMu);
                m_cv.wait_for(lk, std::chrono::milliseconds(currentBackoffMs),
                              [this] { return !m_running.load(); });
            }
            if (!m_running.load()) break;

            ++m_reconnectAttempts;
            ConnectionOptions opts;
            opts.connectTimeoutMs = std::min(m_config.heartbeatTimeoutMs, 2000u);
            opts.sendTimeoutMs    = std::min(m_config.heartbeatTimeoutMs, 3000u);
            opts.recvTimeoutMs    = std::min(m_config.heartbeatTimeoutMs, 3000u);

            if (m_client.connect(m_host, m_port, opts)) {
                if (m_client.ping()) {
                    consecutiveFailures = 0;
                    currentBackoffMs    = m_config.initialBackoffMs;
                    transitionTo(WorkerHealthState::Healthy);
                    break;
                } else {
                    m_client.disconnect();
                }
            }

            // Exponential backoff with clamp
            currentBackoffMs = std::min(
                m_config.maxBackoffMs,
                static_cast<uint32_t>(currentBackoffMs * m_config.backoffMultiplier));
            break;
        }
        }
    }
}

} // namespace dgpu::networking
