// apps/host/workload_cost_model.hpp
// Comprehensive workload-aware execution-cost model for Phase 2.1.
// Predicts 7-stage turnaround latency across all execution strategies
// (HostOnly, WorkerOnly, SpatiallyDistributed, IndependentBatch) and
// selects the optimal execution strategy with automatic local fallback.
#pragma once

#include "protocol/message.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dgpu::host {

// ─── Execution Strategy ───────────────────────────────────────────────────────

enum class ExecutionStrategy : uint32_t {
    HostOnly = 0,             // 100% on local GPU (RTX 3050)
    WorkerOnly = 1,           // 100% on remote GPU (GTX 1650 Ti)
    SpatiallyDistributed = 2, // Spatial halo partitioning across both GPUs
    IndependentBatch = 3      // Concurrently distributed across independent task queues
};

[[nodiscard]] const char* strategyToString(ExecutionStrategy strat) noexcept;

// ─── Workload Key ─────────────────────────────────────────────────────────────
// Uniquely identifies a compute workload by kernel type and resolution.

struct WorkloadKey {
    protocol::TaskType type{protocol::TaskType::Blur};
    uint32_t width{0};
    uint32_t height{0};

    [[nodiscard]] bool operator==(const WorkloadKey& other) const noexcept {
        return type == other.type && width == other.width && height == other.height;
    }
};

struct WorkloadKeyHash {
    [[nodiscard]] std::size_t operator()(const WorkloadKey& k) const noexcept {
        std::size_t h1 = std::hash<uint32_t>{}(static_cast<uint32_t>(k.type));
        std::size_t h2 = std::hash<uint32_t>{}(k.width);
        std::size_t h3 = std::hash<uint32_t>{}(k.height);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

// ─── Fine-Grained 7-Stage Latency Breakdown ───────────────────────────────────

struct StageCostBreakdown {
    double queueDelayUs{0.0};         // Wait time in dispatch queue before processing
    double dispatchOverheadUs{0.0};   // Thread switch, message packing, launch overhead
    double compressCpuUs{0.0};        // Software compression latency (send)
    double wireTransferSendUs{0.0};   // Network serialization & send transfer
    double gpuExecutionUs{0.0};       // Core GPU kernel computation
    double gpuReadbackUs{0.0};        // VRAM readback to host memory
    double wireTransferReturnUs{0.0}; // Network transfer of output result
    double decompressCpuUs{0.0};      // Software decompression latency (recv)
    double stitchOverheadUs{0.0};     // Halo slab extraction & frame reassembly
    double totalTurnaroundUs{0.0};    // End-to-end predicted turnaround time
};

// ─── Strategy Evaluation & Comparison ────────────────────────────────────────

struct StrategyEvaluation {
    ExecutionStrategy strategy{ExecutionStrategy::HostOnly};
    StageCostBreakdown breakdown;
    bool isEligible{true};
    std::string rejectionReason;   // e.g. "Worker lacks required capability"
    float spatialSplitRatio{1.0f}; // Fraction on Host if SpatiallyDistributed
};

struct MultiStrategyComparison {
    WorkloadKey key;
    std::vector<StrategyEvaluation> evaluatedStrategies;
    ExecutionStrategy optimalStrategy{ExecutionStrategy::HostOnly};
    double predictedOptimalLatencyUs{0.0};
    double fastestSingleDeviceBaselineUs{0.0}; // min(HostOnly, WorkerOnly)
    double predictedSpeedupVsBaseline{1.0};
    bool isLocalFallback{false};
    std::string decisionRationale;
};

// ─── Completed Job Execution Sample ──────────────────────────────────────────

struct ExecutionFeedback {
    protocol::TaskType type{protocol::TaskType::Blur};
    uint32_t width{0};
    uint32_t height{0};
    ExecutionStrategy strategy{ExecutionStrategy::HostOnly};

    // Measured components (in microseconds)
    uint64_t queueWaitUs{0};
    uint64_t dispatchUs{0};
    uint64_t gpuExecUs{0};
    uint64_t gpuUploadUs{0};
    uint64_t gpuReadbackUs{0};
    uint64_t networkRttUs{0};
    uint64_t compressUs{0};
    uint64_t decompressUs{0};
    uint64_t splitUs{0};
    uint64_t stitchUs{0};
    uint64_t wallClockUs{0};

    // Payload stats
    size_t uncompressedSendBytes{0};
    size_t compressedSendBytes{0};
    size_t uncompressedRecvBytes{0};
    size_t compressedRecvBytes{0};
};

// ─── WorkloadCostModel ───────────────────────────────────────────────────────

class WorkloadCostModel {
public:
    WorkloadCostModel();
    ~WorkloadCostModel();

    WorkloadCostModel(const WorkloadCostModel&) = delete;
    WorkloadCostModel& operator=(const WorkloadCostModel&) = delete;

    // Link and network configuration
    void setLinkBandwidthBps(uint64_t bps) noexcept;
    [[nodiscard]] uint64_t linkBandwidthBps() const noexcept;

    void setBaseRttUs(uint64_t rttUs) noexcept;
    [[nodiscard]] uint64_t baseRttUs() const noexcept;

    // Compression enablement
    void setCompressionEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isCompressionEnabled() const noexcept;

    // Worker capabilities registration
    void setWorkerCapabilities(const protocol::WorkerCapabilities& caps);
    void clearWorkerCapabilities() noexcept;

    // Fallback tolerance threshold (e.g. 0.05 = remote must beat single GPU by >= 5%)
    void setFallbackTolerance(double tolerance) noexcept;
    [[nodiscard]] double fallbackTolerance() const noexcept;

    // ── Core Scheduling Evaluation ────────────────────────────────────────────
    // Compares HostOnly, WorkerOnly, SpatiallyDistributed, and IndependentBatch,
    // selecting the fastest strategy with automatic local fallback.
    [[nodiscard]] MultiStrategyComparison evaluateStrategies(protocol::TaskType type, uint32_t width, uint32_t height,
                                                             size_t payloadBytes = 0, uint32_t localQueueDepth = 0,
                                                             uint32_t remoteQueueDepth = 0, bool remoteAvailable = true,
                                                             uint32_t batchSize = 1) const;

    // Convenience overload taking TaskRequest
    [[nodiscard]] MultiStrategyComparison evaluateStrategies(const protocol::TaskRequest& req,
                                                             uint32_t localQueueDepth = 0,
                                                             uint32_t remoteQueueDepth = 0, bool remoteAvailable = true,
                                                             uint32_t batchSize = 1) const;

    // ── Continuous Empirical Feedback ────────────────────────────────────────
    // Updates internal estimates and EMAs using actual completed job metrics.
    void recordFeedback(const ExecutionFeedback& feedback);

    // Convenience recording methods for individual execution paths
    void recordLocalSuccess(protocol::TaskType type, uint32_t width, uint32_t height, uint64_t gpuUs, uint64_t totalUs,
                            uint64_t queueWaitUs = 0);

    void recordRemoteSuccess(protocol::TaskType type, uint32_t width, uint32_t height, uint64_t remoteGpuUs,
                             uint64_t networkRttUs, uint64_t totalTurnaroundUs, size_t uncompressedBytes = 0,
                             size_t compressedBytes = 0, uint64_t compressUs = 0, uint64_t decompressUs = 0,
                             uint64_t queueWaitUs = 0);

    void recordPartitionSuccess(protocol::TaskType type, uint32_t width, uint32_t height, uint64_t splitUs,
                                uint64_t stitchUs, uint64_t wallClockUs);

    // Query sample count for a specific workload
    [[nodiscard]] uint64_t sampleCount(protocol::TaskType type, uint32_t width, uint32_t height) const;

    // Reset all learned profiles to cold-start defaults
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::host
