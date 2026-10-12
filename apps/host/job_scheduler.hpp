// apps/host/job_scheduler.hpp
// Decides where each task runs (local vs remote) and maintains running metrics.
#pragma once

#include "protocol/message.hpp"
#include "task_runtime/task_runtime.hpp"

#include <cstdint>
#include <memory>

namespace dgpu::host {

// ─── SchedulerMetrics — updated after every completed task ───────────────────

struct ResolutionMetrics {
    double emaLocalTotalUs  = 0.0;
    double emaRemoteGpuUs   = 0.0;
    double emaNetworkRttUs  = 0.0;
    uint64_t localSamples   = 0;
    uint64_t remoteSamples  = 0;
};

struct SchedulerMetrics {
    // Rolling exponential moving averages (EMA, alpha = 0.1) across all workloads
    double emaLocalUs   = 0.0;    // Local end-to-end execution time
    double emaRemoteUs  = 0.0;    // Remote GPU execution time (worker-reported)
    double emaNetworkUs = 0.0;    // Network round-trip time
    double emaPayloadKb = 0.0;    // Payload size in KB

    uint64_t localCount    = 0;
    uint64_t remoteCount   = 0;
    uint64_t fallbackCount = 0;  // Remote wanted but circuit open → ran locally
    uint64_t localSamples  = 0;
    uint64_t remoteSamples = 0;
};

// ─── JobScheduler ────────────────────────────────────────────────────────────
// Wraps the task_runtime::TaskQueue with adaptive site selection and
// circuit-breaker integration.

class JobScheduler {
public:
    JobScheduler();
    ~JobScheduler();

    JobScheduler(const JobScheduler&)            = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    // Must be called before submit().
    void setLocalExecutor(task_runtime::LocalExecutor exec);
    void setRemoteExecutor(task_runtime::RemoteExecutor exec);

    // Start worker threads.
    void start(uint32_t workerThreads = 2);
    void stop();

    // Submit a task.  The scheduler decides local vs remote unless forceSite is set.
    [[nodiscard]] task_runtime::JobHandle
    submit(protocol::TaskRequest req,
           std::optional<task_runtime::ExecutionSite> forceSite = std::nullopt);

    // Call after each completed remote task (updates EMA and circuit breaker).
    void recordRemoteSuccess(uint64_t remoteGpuUs, uint64_t networkUs);
    void recordRemoteSuccess(uint32_t width, uint32_t height, uint64_t remoteGpuUs, uint64_t networkUs);
    void recordRemoteFailure();

    // Call after each completed local task (updates EMA).
    void recordLocalSuccess(uint64_t localTotalUs, uint64_t payloadBytes);
    void recordLocalSuccess(uint32_t width, uint32_t height, uint64_t localTotalUs, uint64_t payloadBytes);

    [[nodiscard]] const SchedulerMetrics& metrics() const noexcept;
    [[nodiscard]] task_runtime::CircuitBreaker::State circuitState() const noexcept;

    // Link bandwidth and network latency configuration (Priority 1 analytical cost model)
    void setLinkBandwidthBps(uint64_t bytesPerSec);
    [[nodiscard]] uint64_t linkBandwidthBps() const noexcept;
    void setBaseRttUs(uint64_t rttUs);
    [[nodiscard]] uint64_t baseRttUs() const noexcept;

    // Analytical estimate for remote turnaround time (transfer in + remote GPU + transfer out)
    [[nodiscard]] double estimateRemoteTurnaroundUs(uint32_t width, uint32_t height, uint64_t payloadBytes) const noexcept;

private:
    task_runtime::ExecutionSite decide(const protocol::TaskRequest& req) const noexcept;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::host
