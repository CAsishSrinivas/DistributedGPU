// apps/host/workload_partitioner.hpp
// Partitions 2D image compute workloads across local and remote GPUs
// with halo/apron margins for seamless parallel execution.
#pragma once

#include "protocol/message.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
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

// ─── Apron & Partition Calibration Cache (Phase 2.5) ─────────────────────────

struct ApronCacheKey {
    protocol::TaskType type{protocol::TaskType::Blur};
    uint32_t           width{0};
    uint32_t           height{0};
    uint64_t           bandwidthTier{0}; // Quantized bandwidth tier

    bool operator==(const ApronCacheKey& o) const noexcept {
        return type == o.type && width == o.width && height == o.height && bandwidthTier == o.bandwidthTier;
    }
};

struct ApronCacheKeyHash {
    size_t operator()(const ApronCacheKey& k) const noexcept {
        size_t h = std::hash<uint32_t>{}(static_cast<uint32_t>(k.type));
        h ^= std::hash<uint32_t>{}(k.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint64_t>{}(k.bandwidthTier) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct ApronCacheEntry {
    float    calibratedAlpha{0.5f};
    uint32_t apronRadius{9};
    double   emaLocalUs{0.0};
    double   emaRemoteUs{0.0};
    double   emaWallUs{0.0};
    uint64_t hitCount{0};
    uint64_t updateCount{0};
    std::chrono::steady_clock::time_point lastAccess;
};

struct ApronCacheStats {
    uint64_t hits{0};
    uint64_t misses{0};
    uint64_t updates{0};
    size_t   entryCount{0};
};

class ApronCache {
public:
    ApronCache() = default;
    ~ApronCache() = default;

    [[nodiscard]] std::optional<float> lookupAlpha(protocol::TaskType type,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint64_t bandwidthBps) const noexcept;

    [[nodiscard]] std::optional<ApronCacheEntry> getEntry(protocol::TaskType type,
                                                          uint32_t width,
                                                          uint32_t height,
                                                          uint64_t bandwidthBps) const noexcept;

    void put(protocol::TaskType type,
             uint32_t width,
             uint32_t height,
             uint64_t bandwidthBps,
             float alpha,
             uint32_t apronRadius,
             double localUs = 0.0,
             double remoteUs = 0.0,
             double wallUs = 0.0) noexcept;

    // Adaptively shifts alpha using EMA and runtime execution latency feedback
    float recordFeedback(protocol::TaskType type,
                         uint32_t width,
                         uint32_t height,
                         uint64_t bandwidthBps,
                         double actualLocalUs,
                         double actualRemoteUs,
                         double actualWallUs,
                         float learningRate = 0.15f) noexcept;

    void clear() noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] ApronCacheStats stats() const noexcept;
    void resetStats() noexcept;

    static uint64_t quantizeBandwidth(uint64_t bps) noexcept {
        constexpr uint64_t kBucket = 5'000'000; // 5 Mbps bucket
        if (bps < kBucket) return kBucket;
        return (bps / kBucket) * kBucket;
    }

private:
    mutable std::mutex m_mu;
    mutable std::unordered_map<ApronCacheKey, ApronCacheEntry, ApronCacheKeyHash> m_entries;
    mutable ApronCacheStats m_stats;
};

// ─── WorkloadPartitioner ─────────────────────────────────────────────────────

class WorkloadPartitioner {
public:
    WorkloadPartitioner() = default;
    ~WorkloadPartitioner() = default;

    // Determines appropriate halo apron radius based on kernel type & intensity
    [[nodiscard]] static uint32_t
    getApronRadiusForTask(protocol::TaskType type, uint32_t intensity = 0) noexcept;

    // Global singleton ApronCache instance
    [[nodiscard]] static ApronCache& globalApronCache() noexcept;

    // Evaluates candidate splits to minimize wall-clock latency,
    // automatically falling back to 100% local whenever offloading would be slower.
    // If cache is provided, checks and updates calibrated alpha values.
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
                             uint32_t apronRadius = 0,
                             double compressCpuUsPerByte = 0.002,
                             double decompressCpuUsPerByte = 0.0006,
                             protocol::TaskType type = protocol::TaskType::Blur,
                             ApronCache* cache = nullptr) noexcept;

    // Backward-compatible split ratio calculation
    [[nodiscard]] static float
    calculateOptimalSplitRatio(uint32_t width,
                               uint32_t height,
                               double localGpuTotalUs,
                               double remoteGpuTotalUs,
                               uint64_t linkBandwidthBps,
                               uint64_t baseRttUs = 1000,
                               float compressionRatio = 1.0f,
                               uint32_t apronRadius = 0) noexcept;

    // Splits an input task into two horizontal slabs with apron/halo margins.
    // splitRatio: fraction of total rows assigned to Partition 0 (default: 0.50f)
    // apronRadius: convolution kernel radius in pixels (default: 0 = auto-detect from kernel)
    [[nodiscard]] static std::pair<SpatialPartition, SpatialPartition>
    createSlabPartitions(const protocol::TaskRequest& fullReq,
                         float splitRatio = 0.50f,
                         uint32_t apronRadius = 0);

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
    // Automatically passes runtime execution timing back to ApronCache for dynamic calibration.
    [[nodiscard]] static PartitionResult
    executeParallelSlabs(const protocol::TaskRequest& fullReq,
                         LocalExecFn localFn,
                         RemoteExecFn remoteFn,
                         float splitRatio = 0.50f,
                         uint32_t apronRadius = 0,
                         ApronCache* cache = nullptr,
                         uint64_t linkBandwidthBps = 12'500'000);
};

} // namespace dgpu::host
