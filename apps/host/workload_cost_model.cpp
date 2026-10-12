// apps/host/workload_cost_model.cpp
// Implementation of WorkloadCostModel for Phase 2.1.
#include "workload_cost_model.hpp"

#include "workload_partitioner.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace dgpu::host {

static constexpr double DEFAULT_EMA_ALPHA = 0.15;

static double emaUpdate(double prev, double sample, double alpha = DEFAULT_EMA_ALPHA) noexcept {
    if (prev <= 0.0) return sample; // Cold-start initialization
    return alpha * sample + (1.0 - alpha) * prev;
}

const char* strategyToString(ExecutionStrategy strat) noexcept {
    switch (strat) {
    case ExecutionStrategy::HostOnly: return "HostOnly (Local RTX 3050)";
    case ExecutionStrategy::WorkerOnly: return "WorkerOnly (Remote GTX 1650 Ti)";
    case ExecutionStrategy::SpatiallyDistributed: return "SpatiallyDistributed (Halo Partition)";
    case ExecutionStrategy::IndependentBatch: return "IndependentBatch (Concurrent Steal)";
    }
    return "Unknown";
}

// ─── Internal Profile Structs ────────────────────────────────────────────────

struct DeviceProfile {
    double emaGpuExecUs{0.0};
    double emaUploadUs{0.0};
    double emaReadbackUs{0.0};
    double emaDispatchUs{100.0}; // Default 100 us context switch & enqueue
    uint64_t sampleCount{0};
};

struct WorkloadProfile {
    DeviceProfile host;
    DeviceProfile worker;
    double emaSplitUs{300.0};
    double emaStitchUs{500.0};
    uint64_t partitionSamples{0};
};

struct CodecStats {
    double compressUsPerByte{0.002};    // ~500 MB/s software LZ4 compression
    double decompressUsPerByte{0.0006}; // ~1.6 GB/s software LZ4 decompression
    float compressionRatioSend{1.0f};
    float compressionRatioRecv{1.0f};
    uint64_t sampleCount{0};
};

// ─── WorkloadCostModel::Impl ─────────────────────────────────────────────────

struct WorkloadCostModel::Impl {
    mutable std::mutex mu;

    // Workload history keyed by (TaskType, width, height)
    std::unordered_map<WorkloadKey, WorkloadProfile, WorkloadKeyHash> profiles;

    // System parameters
    uint64_t linkBandwidthBps{12'500'000}; // Default: 100 Mbps (~12.5 MB/s)
    uint64_t baseRttUs{1000};              // Default: 1.0 ms base ping
    bool compressionEnabled{true};
    double fallbackTolerance{0.05}; // Remote/dist must beat single GPU by >= 5%
    std::optional<protocol::WorkerCapabilities> workerCaps;

    CodecStats codec;

    // Helper: default execution estimate for unmeasured workloads based on pixel count
    [[nodiscard]] double estimateInitialHostGpuUs(protocol::TaskType type, uint32_t width,
                                                  uint32_t height) const noexcept {
        const double pixels = static_cast<double>(width) * height;
        switch (type) {
        case protocol::TaskType::Blur:
            // Gaussian blur with radius 9 (convolution passes)
            return (pixels / (1024.0 * 1024.0)) * 25'000.0; // ~25 ms for 1024x1024 on RTX 3050
        case protocol::TaskType::EdgeDetection:
            // 3x3 Sobel edge detection
            return (pixels / (1024.0 * 1024.0)) * 8'000.0; // ~8 ms for 1024x1024 on RTX 3050
        }
        return (pixels / (1024.0 * 1024.0)) * 20'000.0;
    }

    [[nodiscard]] double estimateInitialWorkerGpuUs(protocol::TaskType type, uint32_t width,
                                                    uint32_t height) const noexcept {
        // GTX 1650 Ti is roughly 1.15x - 1.25x the duration of RTX 3050
        return estimateInitialHostGpuUs(type, width, height) * 1.20;
    }
};

WorkloadCostModel::WorkloadCostModel() : m_impl(std::make_unique<Impl>()) {}
WorkloadCostModel::~WorkloadCostModel() = default;

void WorkloadCostModel::setLinkBandwidthBps(uint64_t bps) noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->linkBandwidthBps = (bps > 0) ? bps : 12'500'000;
}

uint64_t WorkloadCostModel::linkBandwidthBps() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->linkBandwidthBps;
}

void WorkloadCostModel::setBaseRttUs(uint64_t rttUs) noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->baseRttUs = rttUs;
}

uint64_t WorkloadCostModel::baseRttUs() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->baseRttUs;
}

void WorkloadCostModel::setCompressionEnabled(bool enabled) noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->compressionEnabled = enabled;
}

bool WorkloadCostModel::isCompressionEnabled() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->compressionEnabled;
}

void WorkloadCostModel::setWorkerCapabilities(const protocol::WorkerCapabilities& caps) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->workerCaps = caps;
}

void WorkloadCostModel::clearWorkerCapabilities() noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->workerCaps.reset();
}

void WorkloadCostModel::setFallbackTolerance(double tolerance) noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->fallbackTolerance = std::clamp(tolerance, 0.0, 0.50);
}

double WorkloadCostModel::fallbackTolerance() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->fallbackTolerance;
}

void WorkloadCostModel::reset() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->profiles.clear();
    m_impl->codec = CodecStats{};
}

uint64_t WorkloadCostModel::sampleCount(protocol::TaskType type, uint32_t width, uint32_t height) const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    WorkloadKey key{type, width, height};
    auto it = m_impl->profiles.find(key);
    if (it == m_impl->profiles.end()) return 0;
    return it->second.host.sampleCount + it->second.worker.sampleCount;
}

MultiStrategyComparison WorkloadCostModel::evaluateStrategies(const protocol::TaskRequest& req,
                                                              uint32_t localQueueDepth, uint32_t remoteQueueDepth,
                                                              bool remoteAvailable, uint32_t batchSize) const {
    size_t bytes = req.input.size();
    if (bytes == 0 && req.width > 0 && req.height > 0) {
        bytes = static_cast<size_t>(req.width) * req.height * 4;
    }
    return evaluateStrategies(req.type, req.width, req.height, bytes, localQueueDepth, remoteQueueDepth,
                              remoteAvailable, batchSize);
}

MultiStrategyComparison WorkloadCostModel::evaluateStrategies(protocol::TaskType type, uint32_t width, uint32_t height,
                                                              size_t payloadBytes, uint32_t localQueueDepth,
                                                              uint32_t remoteQueueDepth, bool remoteAvailable,
                                                              uint32_t batchSize) const {
    std::lock_guard<std::mutex> lk(m_impl->mu);

    WorkloadKey key{type, width, height};
    MultiStrategyComparison result;
    result.key = key;

    if (payloadBytes == 0) {
        payloadBytes = static_cast<size_t>(width) * height * 4;
    }
    const double rawBytes = static_cast<double>(payloadBytes);

    // Retrieve or estimate workload profiles
    auto it = m_impl->profiles.find(key);
    double hostGpuUs = (it != m_impl->profiles.end() && it->second.host.sampleCount > 0)
                           ? it->second.host.emaGpuExecUs
                           : m_impl->estimateInitialHostGpuUs(type, width, height);

    double hostUploadUs = (it != m_impl->profiles.end() && it->second.host.sampleCount > 0)
                              ? it->second.host.emaUploadUs
                              : (rawBytes / 12'000'000'000.0) * 1'000'000.0; // PCIe 4.0 ~12 GB/s

    double hostReadbackUs = (it != m_impl->profiles.end() && it->second.host.sampleCount > 0)
                                ? it->second.host.emaReadbackUs
                                : (rawBytes / 6'000'000'000.0) * 1'000'000.0; // PCIe readback ~6 GB/s

    double hostDispatchUs =
        (it != m_impl->profiles.end() && it->second.host.sampleCount > 0) ? it->second.host.emaDispatchUs : 100.0;

    double workerGpuUs = (it != m_impl->profiles.end() && it->second.worker.sampleCount > 0)
                             ? it->second.worker.emaGpuExecUs
                             : m_impl->estimateInitialWorkerGpuUs(type, width, height);

    double workerUploadUs = (it != m_impl->profiles.end() && it->second.worker.sampleCount > 0)
                                ? it->second.worker.emaUploadUs
                                : (rawBytes / 8'000'000'000.0) * 1'000'000.0; // PCIe 3.0 ~8 GB/s

    double workerReadbackUs = (it != m_impl->profiles.end() && it->second.worker.sampleCount > 0)
                                  ? it->second.worker.emaReadbackUs
                                  : (rawBytes / 4'000'000'000.0) * 1'000'000.0;

    double splitUs = (it != m_impl->profiles.end() && it->second.partitionSamples > 0) ? it->second.emaSplitUs : 300.0;
    double stitchUs =
        (it != m_impl->profiles.end() && it->second.partitionSamples > 0) ? it->second.emaStitchUs : 500.0;

    // Check remote capabilities
    bool remoteCapable = remoteAvailable;
    std::string remoteIneligibleReason;
    if (!remoteAvailable) {
        remoteCapable = false;
        remoteIneligibleReason = "Remote worker is offline or disconnected";
    } else if (m_impl->workerCaps.has_value()) {
        if (!m_impl->workerCaps->supportsTask(type, width, height)) {
            remoteCapable = false;
            remoteIneligibleReason = std::format("Worker capabilities reject task type {} at {}x{}",
                                                 static_cast<uint32_t>(type), width, height);
        }
    }

    // Codec parameters
    const float compRatioSend = m_impl->compressionEnabled ? std::max(1.0f, m_impl->codec.compressionRatioSend) : 1.0f;
    const float compRatioRecv = m_impl->compressionEnabled ? std::max(1.0f, m_impl->codec.compressionRatioRecv) : 1.0f;
    const double compCpuUsPerByte = m_impl->compressionEnabled ? m_impl->codec.compressUsPerByte : 0.0;
    const double decompCpuUsPerByte = m_impl->compressionEnabled ? m_impl->codec.decompressUsPerByte : 0.0;

    // Wire transfer latency calculations
    const double linkBps = static_cast<double>(m_impl->linkBandwidthBps);
    const double baseRttUs = static_cast<double>(m_impl->baseRttUs);

    auto computeWireUs = [&](double bytes, float ratio) noexcept {
        double transferred = bytes / static_cast<double>(ratio);
        return (transferred / linkBps) * 1'000'000.0 + (baseRttUs * 0.5);
    };

    // ──────────────────────────────────────────────────────────────────────────
    // 1. HostOnly Strategy
    // ──────────────────────────────────────────────────────────────────────────
    StrategyEvaluation evalHost;
    evalHost.strategy = ExecutionStrategy::HostOnly;
    evalHost.isEligible = true;
    evalHost.breakdown.queueDelayUs = static_cast<double>(localQueueDepth) * hostGpuUs;
    evalHost.breakdown.dispatchOverheadUs = hostDispatchUs;
    evalHost.breakdown.compressCpuUs = 0.0;
    evalHost.breakdown.wireTransferSendUs = 0.0;
    evalHost.breakdown.gpuExecutionUs = hostGpuUs;
    evalHost.breakdown.gpuReadbackUs = hostReadbackUs;
    evalHost.breakdown.wireTransferReturnUs = 0.0;
    evalHost.breakdown.decompressCpuUs = 0.0;
    evalHost.breakdown.stitchOverheadUs = 0.0;
    evalHost.breakdown.totalTurnaroundUs = evalHost.breakdown.queueDelayUs + evalHost.breakdown.dispatchOverheadUs +
                                           hostUploadUs + evalHost.breakdown.gpuExecutionUs +
                                           evalHost.breakdown.gpuReadbackUs;
    result.evaluatedStrategies.push_back(evalHost);

    // ──────────────────────────────────────────────────────────────────────────
    // 2. WorkerOnly Strategy
    // ──────────────────────────────────────────────────────────────────────────
    StrategyEvaluation evalWorker;
    evalWorker.strategy = ExecutionStrategy::WorkerOnly;
    if (!remoteCapable) {
        evalWorker.isEligible = false;
        evalWorker.rejectionReason = remoteIneligibleReason;
        evalWorker.breakdown.totalTurnaroundUs = 1e9;
    } else {
        evalWorker.isEligible = true;
        evalWorker.breakdown.queueDelayUs = static_cast<double>(remoteQueueDepth) * workerGpuUs;
        evalWorker.breakdown.dispatchOverheadUs = hostDispatchUs;
        evalWorker.breakdown.compressCpuUs = rawBytes * compCpuUsPerByte;
        evalWorker.breakdown.wireTransferSendUs = computeWireUs(rawBytes, compRatioSend);
        evalWorker.breakdown.gpuExecutionUs = workerGpuUs;
        evalWorker.breakdown.gpuReadbackUs = workerReadbackUs;
        evalWorker.breakdown.wireTransferReturnUs = computeWireUs(rawBytes, compRatioRecv);
        evalWorker.breakdown.decompressCpuUs = (rawBytes / compRatioRecv) * decompCpuUsPerByte;
        evalWorker.breakdown.stitchOverheadUs = 0.0;
        evalWorker.breakdown.totalTurnaroundUs =
            evalWorker.breakdown.queueDelayUs + evalWorker.breakdown.dispatchOverheadUs +
            evalWorker.breakdown.compressCpuUs + evalWorker.breakdown.wireTransferSendUs + workerUploadUs +
            evalWorker.breakdown.gpuExecutionUs + evalWorker.breakdown.gpuReadbackUs +
            evalWorker.breakdown.wireTransferReturnUs + evalWorker.breakdown.decompressCpuUs;
    }
    result.evaluatedStrategies.push_back(evalWorker);

    // ──────────────────────────────────────────────────────────────────────────
    // 3. SpatiallyDistributed Strategy (Halo Partitioning)
    // ──────────────────────────────────────────────────────────────────────────
    StrategyEvaluation evalDist;
    evalDist.strategy = ExecutionStrategy::SpatiallyDistributed;
    if (!remoteCapable) {
        evalDist.isEligible = false;
        evalDist.rejectionReason = remoteIneligibleReason;
        evalDist.breakdown.totalTurnaroundUs = 1e9;
    } else {
        evalDist.isEligible = true;
        // Evaluate candidate partition split ratio using WorkloadPartitioner
        PartitionDecision partDecision = WorkloadPartitioner::calculateOptimalDecision(
            width, height, hostGpuUs + hostUploadUs + hostReadbackUs, workerGpuUs + workerUploadUs + workerReadbackUs,
            m_impl->linkBandwidthBps, m_impl->baseRttUs, compRatioSend, splitUs, stitchUs, compRatioRecv,
            WorkloadPartitioner::getApronRadiusForTask(type),
            compCpuUsPerByte, decompCpuUsPerByte,
            type, &WorkloadPartitioner::globalApronCache());

        evalDist.spatialSplitRatio = partDecision.splitRatio;

        if (partDecision.isLocalFallback) {
            evalDist.breakdown = evalHost.breakdown;
            evalDist.breakdown.stitchOverheadUs = 0.0;
            evalDist.rejectionReason = "Distributed partition predicted slower than local-only";
        } else {
            evalDist.breakdown.queueDelayUs = std::max(static_cast<double>(localQueueDepth) * hostGpuUs,
                                                       static_cast<double>(remoteQueueDepth) * workerGpuUs);
            evalDist.breakdown.dispatchOverheadUs = hostDispatchUs;
            evalDist.breakdown.compressCpuUs = partDecision.predictedCompressUs;
            evalDist.breakdown.wireTransferSendUs = partDecision.predictedWireSendUs;
            evalDist.breakdown.gpuExecutionUs =
                std::max(partDecision.predictedLocalBranchUs, partDecision.predictedWorkerGpuUs);
            evalDist.breakdown.gpuReadbackUs = hostReadbackUs * partDecision.splitRatio;
            evalDist.breakdown.wireTransferReturnUs = partDecision.predictedWireRecvUs;
            evalDist.breakdown.decompressCpuUs = partDecision.predictedDecompressUs;
            evalDist.breakdown.stitchOverheadUs = partDecision.predictedStitchUs;
            evalDist.breakdown.totalTurnaroundUs = evalDist.breakdown.queueDelayUs +
                                                   evalDist.breakdown.dispatchOverheadUs +
                                                   partDecision.predictedWallClockUs;
        }
    }
    result.evaluatedStrategies.push_back(evalDist);

    // ──────────────────────────────────────────────────────────────────────────
    // 4. IndependentBatch Strategy
    // ──────────────────────────────────────────────────────────────────────────
    StrategyEvaluation evalBatch;
    evalBatch.strategy = ExecutionStrategy::IndependentBatch;
    if (!remoteCapable || batchSize <= 1) {
        evalBatch.isEligible = false;
        evalBatch.rejectionReason =
            (batchSize <= 1) ? "Batch size must be >= 2 for independent concurrent execution" : remoteIneligibleReason;
        evalBatch.breakdown.totalTurnaroundUs = 1e9;
    } else {
        evalBatch.isEligible = true;
        // Optimal batch proportion: divide tasks according to relative speed
        double hostSingleUs = evalHost.breakdown.totalTurnaroundUs;
        double workerSingleUs = evalWorker.breakdown.totalTurnaroundUs;
        double hostRate = (hostSingleUs > 0) ? (1.0 / hostSingleUs) : 1.0;
        double workerRate = (workerSingleUs > 0) ? (1.0 / workerSingleUs) : 1.0;
        double totalRate = hostRate + workerRate;

        double hostShare = hostRate / totalRate;
        double hostTasks = std::round(static_cast<double>(batchSize) * hostShare);
        double workerTasks = static_cast<double>(batchSize) - hostTasks;

        double makespanHost = hostTasks * hostSingleUs;
        double makespanWorker = workerTasks * workerSingleUs;
        double makespanDist = std::max(makespanHost, makespanWorker);

        evalBatch.breakdown.queueDelayUs = 0.0;
        evalBatch.breakdown.dispatchOverheadUs = hostDispatchUs * static_cast<double>(batchSize);
        evalBatch.breakdown.gpuExecutionUs = makespanDist;
        evalBatch.breakdown.totalTurnaroundUs = makespanDist;
    }
    result.evaluatedStrategies.push_back(evalBatch);

    // ──────────────────────────────────────────────────────────────────────────
    // Strategy Decision & Local Fallback Policy
    // ──────────────────────────────────────────────────────────────────────────
    double singleGpuBaseline = evalHost.breakdown.totalTurnaroundUs;
    if (evalWorker.isEligible && evalWorker.breakdown.totalTurnaroundUs < singleGpuBaseline) {
        singleGpuBaseline = evalWorker.breakdown.totalTurnaroundUs;
    }
    result.fastestSingleDeviceBaselineUs = singleGpuBaseline;

    // Find fastest eligible strategy
    ExecutionStrategy bestStrategy = ExecutionStrategy::HostOnly;
    double minLatency = evalHost.breakdown.totalTurnaroundUs;

    for (const auto& eval : result.evaluatedStrategies) {
        if (eval.isEligible && eval.breakdown.totalTurnaroundUs < minLatency) {
            minLatency = eval.breakdown.totalTurnaroundUs;
            bestStrategy = eval.strategy;
        }
    }

    // Enforce Fallback Policy:
    // If the best strategy is remote or distributed, but does NOT beat single GPU baseline
    // by at least the fallback tolerance, or if it is slower than HostOnly, fall back to HostOnly.
    if (bestStrategy != ExecutionStrategy::HostOnly) {
        double threshold = singleGpuBaseline * (1.0 - m_impl->fallbackTolerance);
        if (minLatency >= threshold) {
            result.isLocalFallback = true;
            result.optimalStrategy = ExecutionStrategy::HostOnly;
            result.predictedOptimalLatencyUs = evalHost.breakdown.totalTurnaroundUs;
            result.predictedSpeedupVsBaseline = 1.0;
            result.decisionRationale = std::format(
                "Local fallback triggered: {} predicted turnaround ({:.1f} us) does not improve upon "
                "fastest single GPU baseline ({:.1f} us) within tolerance {:.1f}%. Executing locally.",
                strategyToString(bestStrategy), minLatency, singleGpuBaseline, m_impl->fallbackTolerance * 100.0);
            return result;
        }
    }

    result.optimalStrategy = bestStrategy;
    result.predictedOptimalLatencyUs = minLatency;
    result.predictedSpeedupVsBaseline = (minLatency > 0) ? (singleGpuBaseline / minLatency) : 1.0;
    result.isLocalFallback = (bestStrategy == ExecutionStrategy::HostOnly);
    result.decisionRationale =
        std::format("Selected {} with predicted latency {:.1f} us ({:.2f}x speedup vs single-GPU baseline {:.1f} us).",
                    strategyToString(bestStrategy), minLatency, result.predictedSpeedupVsBaseline, singleGpuBaseline);

    return result;
}

void WorkloadCostModel::recordFeedback(const ExecutionFeedback& feedback) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    WorkloadKey key{feedback.type, feedback.width, feedback.height};
    auto& prof = m_impl->profiles[key];

    if (feedback.strategy == ExecutionStrategy::HostOnly) {
        prof.host.emaGpuExecUs = emaUpdate(prof.host.emaGpuExecUs, static_cast<double>(feedback.gpuExecUs));
        prof.host.emaUploadUs = emaUpdate(prof.host.emaUploadUs, static_cast<double>(feedback.gpuUploadUs));
        prof.host.emaReadbackUs = emaUpdate(prof.host.emaReadbackUs, static_cast<double>(feedback.gpuReadbackUs));
        if (feedback.dispatchUs > 0) {
            prof.host.emaDispatchUs = emaUpdate(prof.host.emaDispatchUs, static_cast<double>(feedback.dispatchUs));
        }
        ++prof.host.sampleCount;
    } else if (feedback.strategy == ExecutionStrategy::WorkerOnly) {
        prof.worker.emaGpuExecUs = emaUpdate(prof.worker.emaGpuExecUs, static_cast<double>(feedback.gpuExecUs));
        prof.worker.emaUploadUs = emaUpdate(prof.worker.emaUploadUs, static_cast<double>(feedback.gpuUploadUs));
        prof.worker.emaReadbackUs = emaUpdate(prof.worker.emaReadbackUs, static_cast<double>(feedback.gpuReadbackUs));
        ++prof.worker.sampleCount;

        // Update codec and wire stats
        if (feedback.uncompressedSendBytes > 0 && feedback.compressedSendBytes > 0) {
            float ratio =
                static_cast<float>(feedback.uncompressedSendBytes) / static_cast<float>(feedback.compressedSendBytes);
            m_impl->codec.compressionRatioSend = static_cast<float>(
                emaUpdate(static_cast<double>(m_impl->codec.compressionRatioSend), static_cast<double>(ratio)));

            if (feedback.compressUs > 0) {
                double rate =
                    static_cast<double>(feedback.compressUs) / static_cast<double>(feedback.uncompressedSendBytes);
                m_impl->codec.compressUsPerByte = emaUpdate(m_impl->codec.compressUsPerByte, rate);
            }
        }

        if (feedback.uncompressedRecvBytes > 0 && feedback.compressedRecvBytes > 0) {
            float ratio =
                static_cast<float>(feedback.uncompressedRecvBytes) / static_cast<float>(feedback.compressedRecvBytes);
            m_impl->codec.compressionRatioRecv = static_cast<float>(
                emaUpdate(static_cast<double>(m_impl->codec.compressionRatioRecv), static_cast<double>(ratio)));

            if (feedback.decompressUs > 0) {
                double rate =
                    static_cast<double>(feedback.decompressUs) / static_cast<double>(feedback.uncompressedRecvBytes);
                m_impl->codec.decompressUsPerByte = emaUpdate(m_impl->codec.decompressUsPerByte, rate);
            }
        }

        if (feedback.networkRttUs > 500 && feedback.uncompressedSendBytes > 0) {
            double totalWireBytes = static_cast<double>(
                feedback.compressedSendBytes > 0 ? feedback.compressedSendBytes + feedback.compressedRecvBytes
                                                 : feedback.uncompressedSendBytes * 2);
            double measuredBps = (totalWireBytes / static_cast<double>(feedback.networkRttUs)) * 1'000'000.0;
            m_impl->linkBandwidthBps =
                static_cast<uint64_t>(emaUpdate(static_cast<double>(m_impl->linkBandwidthBps), measuredBps));
        }
    } else if (feedback.strategy == ExecutionStrategy::SpatiallyDistributed) {
        if (feedback.splitUs > 0) {
            prof.emaSplitUs = emaUpdate(prof.emaSplitUs, static_cast<double>(feedback.splitUs));
        }
        if (feedback.stitchUs > 0) {
            prof.emaStitchUs = emaUpdate(prof.emaStitchUs, static_cast<double>(feedback.stitchUs));
        }
        ++prof.partitionSamples;
    }
}

void WorkloadCostModel::recordLocalSuccess(protocol::TaskType type, uint32_t width, uint32_t height, uint64_t gpuUs,
                                           uint64_t totalUs, uint64_t queueWaitUs) {
    ExecutionFeedback fb;
    fb.type = type;
    fb.width = width;
    fb.height = height;
    fb.strategy = ExecutionStrategy::HostOnly;
    fb.gpuExecUs = gpuUs;
    fb.wallClockUs = totalUs;
    fb.queueWaitUs = queueWaitUs;
    recordFeedback(fb);
}

void WorkloadCostModel::recordRemoteSuccess(protocol::TaskType type, uint32_t width, uint32_t height,
                                            uint64_t remoteGpuUs, uint64_t networkRttUs, uint64_t totalTurnaroundUs,
                                            size_t uncompressedBytes, size_t compressedBytes, uint64_t compressUs,
                                            uint64_t decompressUs, uint64_t queueWaitUs) {
    ExecutionFeedback fb;
    fb.type = type;
    fb.width = width;
    fb.height = height;
    fb.strategy = ExecutionStrategy::WorkerOnly;
    fb.gpuExecUs = remoteGpuUs;
    fb.networkRttUs = networkRttUs;
    fb.wallClockUs = totalTurnaroundUs;
    fb.uncompressedSendBytes = uncompressedBytes;
    fb.compressedSendBytes = compressedBytes;
    fb.compressUs = compressUs;
    fb.decompressUs = decompressUs;
    fb.queueWaitUs = queueWaitUs;
    recordFeedback(fb);
}

void WorkloadCostModel::recordPartitionSuccess(protocol::TaskType type, uint32_t width, uint32_t height,
                                               uint64_t splitUs, uint64_t stitchUs, uint64_t wallClockUs) {
    ExecutionFeedback fb;
    fb.type = type;
    fb.width = width;
    fb.height = height;
    fb.strategy = ExecutionStrategy::SpatiallyDistributed;
    fb.splitUs = splitUs;
    fb.stitchUs = stitchUs;
    fb.wallClockUs = wallClockUs;
    recordFeedback(fb);
}

} // namespace dgpu::host
