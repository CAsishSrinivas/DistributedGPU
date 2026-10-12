// apps/host/batch_scheduler.hpp
// Advanced batch scheduling system:
// 1. Allocation sweep (0/N to N/0) to find optimal static balance.
// 2. Dynamic work-stealing with bounded queue.
// 3. Multi-task size & kernel evaluation (128x128, 256x256, 512x512, Blur, EdgeDetect).
// 4. Detailed per-task timing breakdown, tail latencies (P50, P90, P95, P99, Max),
//    throughput, and queue wait times.
#pragma once

#include "protocol/message.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dgpu::host {

struct TaskLatencyBreakdown {
    uint64_t taskId{0};
    std::string executedOn; // "Host (RTX 3050)" or "Worker (GTX 1650 Ti)"
    double queueWaitMs{0.0};
    double wireTransferMs{0.0};
    double cpuCodecMs{0.0};
    double gpuExecutionMs{0.0};
    double totalTurnaroundMs{0.0};
    bool success{true};
};

struct BatchRunMetrics {
    std::string modeName;
    uint32_t totalTasks{0};
    uint32_t hostTasksExecuted{0};
    uint32_t workerTasksExecuted{0};
    uint32_t failedTasks{0};

    double totalWallClockMs{0.0};
    double throughputTasksPerSec{0.0};
    double speedupVsHostAlone{1.0};

    // Latency distribution across individual tasks
    double p50LatencyMs{0.0};
    double p90LatencyMs{0.0};
    double p95LatencyMs{0.0};
    double p99LatencyMs{0.0};
    double maxLatencyMs{0.0};
    double meanQueueWaitMs{0.0};
    double meanWireTransferMs{0.0};

    // Phase 2.2 Work-Stealing & Load Balancing Metrics
    uint32_t tasksStolenByHost{0};
    uint32_t tasksStolenByWorker{0};
    uint32_t tinyTasksRetainedLocally{0};

    // Phase 2.3 Pipelined Execution Metrics
    double   overlapFactor{1.0};
    double   pipelineSpeedup{1.0};
    uint32_t inFlightDepth{2};

    double maeVsReference{0.0};
    std::vector<TaskLatencyBreakdown> taskDetails;
};

struct SweepPointResult {
    uint32_t hostTaskCount{0};
    uint32_t workerTaskCount{0};
    double totalWallClockMs{0.0};
    double throughputTasksPerSec{0.0};
    double speedupVsHostAlone{1.0};
    double hostBusyMs{0.0};
    double workerBusyMs{0.0};
};

class AdvancedBatchScheduler {
public:
    using LocalExecFn = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;
    using RemoteExecFn = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;

    // ─── 1. Allocation Sweep ──────────────────────────────────────────────────
    // Evaluates static split points from 0/N to N/0 to map the full efficiency curve
    [[nodiscard]] static std::vector<SweepPointResult>
    runAllocationSweep(const std::vector<protocol::TaskRequest>& tasks, LocalExecFn localExec, RemoteExecFn remoteExec,
                       bool remoteAvailable, uint32_t step = 4);

    // ─── 2. Dynamic Work-Stealing with Bounded Queue ──────────────────────────
    // Worker and Host pull dynamically from a concurrent atomic queue with bounded prefetch
    [[nodiscard]] static BatchRunMetrics
    runDynamicWorkStealing(const std::vector<protocol::TaskRequest>& tasks, LocalExecFn localExec,
                           RemoteExecFn remoteExec, bool remoteAvailable, double referenceHostThroughput = 0.0,
                           const std::vector<protocol::TaskResult>& referenceResults = {});

    // ─── 2b. Phase 2.2: Adaptive Work-Stealing Load Balancer ──────────────────
    // Allocates tasks according to observed processing speeds, bounds queues,
    // filters tiny jobs, and dynamically steals work between idle workers.
    [[nodiscard]] static BatchRunMetrics runAdaptiveWorkStealing(
        const std::vector<protocol::TaskRequest>& tasks, LocalExecFn localExec, RemoteExecFn remoteExec,
        bool remoteAvailable, const std::optional<protocol::WorkerCapabilities>& workerCaps = std::nullopt,
        uint64_t linkBandwidthBps = 12'500'000, uint64_t baseRttUs = 1000, double referenceHostThroughput = 0.0,
        const std::vector<protocol::TaskResult>& referenceResults = {});

    // ─── 2c. Phase 2.3: Pipelined Batch Execution ────────────────────────────
    // Overlaps CPU LZ4 compression/decompression, transfers, and GPU computation
    // using bounded in-flight double/multi-buffering.
    [[nodiscard]] static BatchRunMetrics runPipelinedBatch(
        const std::vector<protocol::TaskRequest>& tasks, LocalExecFn localExec, RemoteExecFn remoteExec,
        bool remoteAvailable, uint32_t inFlightDepth = 2, double referenceHostThroughput = 0.0,
        const std::vector<protocol::TaskResult>& referenceResults = {});

    // ─── 3. Static Split Run ──────────────────────────────────────────────────
    [[nodiscard]] static BatchRunMetrics runStaticSplit(const std::vector<protocol::TaskRequest>& tasks,
                                                        uint32_t hostTaskCount, LocalExecFn localExec,
                                                        RemoteExecFn remoteExec, bool remoteAvailable,
                                                        double referenceHostThroughput = 0.0,
                                                        const std::vector<protocol::TaskResult>& referenceResults = {});

    // ─── 4. Host Alone Reference Run ──────────────────────────────────────────
    [[nodiscard]] static BatchRunMetrics runHostOnly(const std::vector<protocol::TaskRequest>& tasks,
                                                     LocalExecFn localExec);

    // ─── Helper: Print Sweep Results Table ────────────────────────────────────
    static void printSweepTable(const std::vector<SweepPointResult>& sweep);

    // ─── Helper: Print Batch Metrics Summary ──────────────────────────────────
    static void printMetricsSummary(const BatchRunMetrics& metrics);
};

} // namespace dgpu::host
