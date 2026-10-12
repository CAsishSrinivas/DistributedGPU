// apps/host/workload_partitioner.hpp
// Partitions 2D image compute workloads across local and remote GPUs
// with halo/apron margins for seamless parallel execution.
#pragma once

#include "protocol/message.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace dgpu::host {

// ─── Detailed 7-Stage End-to-End Timing Breakdown (Priority 2) ───────────────

struct PartitionTiming {
    // Local partition breakdown (Host RTX 3050)
    uint64_t localUploadUs{0};
    uint64_t localGpuUs{0};
    uint64_t localReadbackUs{0};
    uint64_t localTotalUs{0};

    // Remote partition breakdown (Worker GTX 1650 Ti)
    uint64_t remoteNetworkSendUs{0};
    uint64_t remoteUploadUs{0};
    uint64_t remoteGpuUs{0};
    uint64_t remoteReadbackUs{0};
    uint64_t remoteNetworkReturnUs{0};
    uint64_t remoteTotalUs{0};       // Worker internal execution + network roundtrip

    // Compression metrics (Software LZ4 Codec)
    uint64_t compressionCpuUs{0};
    uint64_t decompressionCpuUs{0};
    size_t   uncompressedSendBytes{0};
    size_t   compressedSendBytes{0};
    size_t   compressedRecvBytes{0};
    size_t   uncompressedRecvBytes{0};
    float    compressionRatioSend{1.0f};
    float    compressionRatioRecv{1.0f};

    // Partitioning & stitching overhead
    uint64_t splitUs{0};
    uint64_t stitchUs{0};

    // Total end-to-end wall-clock time from split start to stitch finish
    uint64_t wallClockUs{0};

    // True if scheduler decided offloading was slower and executed 100% locally
    bool isLocalFallback{false};
};

// ─── Cost-Aware Partition Decision (Priority 2) ──────────────────────────────

struct PartitionDecision {
    float splitRatio{1.0f};           // Fraction assigned to local (1.0 = local fallback)
    float bestCandidateRatio{0.50f};  // Best candidate split ratio tested
    double predictedLocalUs{0.0};     // Predicted local finish time
    double predictedRemoteUs{0.0};    // Predicted remote finish time (incl. wire + worker + stitch)
    double predictedWallClockUs{0.0}; // max(predLocal, predRemote) + stitch
    double predictedSingleGpuUs{0.0}; // Time if 100% executed on fastest single GPU
    bool isLocalFallback{false};      // True if offloading is slower than local-only
    float effectiveCompressionRatio{1.0f};
    float effectiveCompressionRatioRecv{1.0f};
    
    // Fine-grained 7-stage cost breakdown
    double predictedLocalBranchUs{0.0};
    double predictedRemoteBranchUs{0.0};
    double predictedWireSendUs{0.0};
    double predictedWireRecvUs{0.0};
    double predictedCompressUs{0.0};
    double predictedDecompressUs{0.0};
    double predictedWorkerGpuUs{0.0};
    double predictedStitchUs{0.0};
    double predictedSpeedupVsBestGpu{1.0};
    bool   predictedCompressionHelps{true};
    std::string rationale;
};

// ─── Spatial Slab Information ────────────────────────────────────────────────

struct SpatialPartition {
    uint32_t partitionIndex{0};
    uint32_t fullWidth{0};
    uint32_t fullHeight{0};

    // Core region (excluding apron)
    uint32_t coreY{0};
    uint32_t coreHeight{0};

    // Extracted input slab (including apron)
    uint32_t inputY{0};
    uint32_t inputHeight{0};
    uint32_t apronTop{0};
    uint32_t apronBottom{0};

    protocol::TaskRequest request;
};

// ─── WorkloadPartitioner ─────────────────────────────────────────────────────

class WorkloadPartitioner {
public:
    WorkloadPartitioner() = default;
    ~WorkloadPartitioner() = default;

    // Evaluates candidate splits to minimize wall-clock latency,
    // automatically falling back to 100% local whenever offloading would be slower.
    [[nodiscard]] static PartitionDecision
    calculateOptimalDecision(uint32_t width,
                             uint32_t height,
                             double localGpuTotalUs,
                             double remoteGpuTotalUs,
                             uint64_t linkBandwidthBps,
                             uint64_t baseRttUs = 1000,
                             float compressionRatio = 1.0f,
                             double splitOverheadUs = 300.0,
                             double stitchOverheadUs = 500.0,
                             float compressionRatioRecv = 1.0f,
                             uint32_t apronRadius = 9,
                             double compressCpuUsPerByte = 0.002,
                             double decompressCpuUsPerByte = 0.0006) noexcept;

    // Backward-compatible split ratio calculation
    [[nodiscard]] static float
    calculateOptimalSplitRatio(uint32_t width,
                               uint32_t height,
                               double localGpuTotalUs,
                               double remoteGpuTotalUs,
                               uint64_t linkBandwidthBps,
                               uint64_t baseRttUs = 1000,
                               float compressionRatio = 1.0f) noexcept;

    // Splits an input task into two horizontal slabs with apron/halo margins.
    // splitRatio: fraction of total rows assigned to Partition 0 (default: 0.50f)
    // apronRadius: convolution kernel radius in pixels (default: 9)
    [[nodiscard]] static std::pair<SpatialPartition, SpatialPartition>
    createSlabPartitions(const protocol::TaskRequest& fullReq,
                         float splitRatio = 0.50f,
                         uint32_t apronRadius = 9);

    // Reassembles two partition results into a single output frame,
    // discarding the apron margins and stitching the core regions seamlessly.
    [[nodiscard]] static std::vector<std::byte>
    stitchSlabPartitions(const SpatialPartition& part0,
                         const protocol::TaskResult& res0,
                         const SpatialPartition& part1,
                         const protocol::TaskResult& res1);

    // Execution callbacks for local and remote dispatch
    using LocalExecFn  = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;
    using RemoteExecFn = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;

    struct PartitionResult {
        std::vector<std::byte> output;
        PartitionTiming        timing;
        bool                   success{false};
    };

    // Executes a spatial partitioned workload concurrently across local and remote executors,
    // collecting complete timing breakdown and producing the seamless stitched output.
    [[nodiscard]] static PartitionResult
    executeParallelSlabs(const protocol::TaskRequest& fullReq,
                         LocalExecFn localFn,
                         RemoteExecFn remoteFn,
                         float splitRatio = 0.50f,
                         uint32_t apronRadius = 9);
};

} // namespace dgpu::host
