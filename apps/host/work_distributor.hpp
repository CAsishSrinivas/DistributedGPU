// apps/host/work_distributor.hpp
// Dynamic Work-Stealing Load Balancer for Phase 2.2.
// Manages per-worker bounded task queues, tracks real-time throughput and queue depth,
// allocates tasks proportionally to observed GPU processing speeds, and allows idle
// workers to steal compatible tasks while preventing tiny-job migration thrashing.
#pragma once

#include "protocol/message.hpp"
#include "workload_cost_model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dgpu::host {

// ─── Worker Runtime Tracking ──────────────────────────────────────────────────

struct WorkerRuntimeStats {
    std::string workerId;
    std::string deviceName;
    uint32_t activeJobs{0};
    uint32_t queuedJobs{0};
    uint32_t completedJobs{0};
    uint32_t stolenJobs{0};
    double emaDurationMs{0.0};
    double throughputTasksPerSec{0.0};
    bool isAvailable{true};
};

// ─── Load Balancer Configuration ──────────────────────────────────────────────

struct WorkDistributorConfig {
    uint32_t maxQueueDepth{32};           // Bounded per-worker queue capacity (backpressure)
    uint32_t stealThreshold{2};           // Only steal when victim queue has >= stealThreshold tasks
    double minComputeUsForRemote{1500.0}; // Tiny-task threshold: retain locally if below
    bool allowWorkStealing{true};
};

// ─── Work-Stealing Execution Metrics ──────────────────────────────────────────

struct WorkDistributorMetrics {
    uint32_t totalTasks{0};
    uint32_t hostTasksExecuted{0};
    uint32_t workerTasksExecuted{0};
    uint32_t tasksStolenByHost{0};
    uint32_t tasksStolenByWorker{0};
    uint32_t tinyTasksRetainedLocally{0};
    uint32_t failedTasks{0};

    double totalWallClockMs{0.0};
    double throughputTasksPerSec{0.0};
    double speedupVsStatic5050{1.0};
    double speedupVsHostAlone{1.0};

    WorkerRuntimeStats hostStats;
    WorkerRuntimeStats workerStats;
};

// ─── DynamicWorkDistributor ──────────────────────────────────────────────────

class DynamicWorkDistributor {
public:
    using TaskExecFn = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;

    explicit DynamicWorkDistributor(WorkDistributorConfig config = {});
    ~DynamicWorkDistributor();

    DynamicWorkDistributor(const DynamicWorkDistributor&) = delete;
    DynamicWorkDistributor& operator=(const DynamicWorkDistributor&) = delete;

    void setWorkerCapabilities(const protocol::WorkerCapabilities& caps);
    void clearWorkerCapabilities() noexcept;

    void setLinkBandwidthBps(uint64_t bps) noexcept;
    void setBaseRttUs(uint64_t rttUs) noexcept;

    // Execute a batch of tasks adaptively using dynamic work-stealing and load balancing
    [[nodiscard]] WorkDistributorMetrics executeBatch(const std::vector<protocol::TaskRequest>& tasks,
                                                      TaskExecFn localExec, TaskExecFn remoteExec, bool remoteAvailable,
                                                      std::vector<protocol::TaskResult>* outResults = nullptr);

    [[nodiscard]] const WorkDistributorConfig& config() const noexcept;
    void setConfig(const WorkDistributorConfig& config) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::host
