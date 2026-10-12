// apps/host/workload_partitioner.cpp
// Slab partitioning and halo stitching for multi-GPU cooperative compute.

#include "workload_partitioner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>

namespace dgpu::host {

// ─── ApronCache Implementation (Phase 2.5) ───────────────────────────────────

std::optional<float> ApronCache::lookupAlpha(protocol::TaskType type,
                                             uint32_t width,
                                             uint32_t height,
                                             uint64_t bandwidthBps) const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    ApronCacheKey key{type, width, height, quantizeBandwidth(bandwidthBps)};
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        ++it->second.hitCount;
        ++m_stats.hits;
        return it->second.calibratedAlpha;
    }
    ++m_stats.misses;
    return std::nullopt;
}

std::optional<ApronCacheEntry> ApronCache::getEntry(protocol::TaskType type,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    uint64_t bandwidthBps) const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    ApronCacheKey key{type, width, height, quantizeBandwidth(bandwidthBps)};
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        ++it->second.hitCount;
        ++m_stats.hits;
        return it->second;
    }
    ++m_stats.misses;
    return std::nullopt;
}

void ApronCache::put(protocol::TaskType type,
                     uint32_t width,
                     uint32_t height,
                     uint64_t bandwidthBps,
                     float alpha,
                     uint32_t apronRadius,
                     double localUs,
                     double remoteUs,
                     double wallUs) noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    ApronCacheKey key{type, width, height, quantizeBandwidth(bandwidthBps)};
    auto& entry = m_entries[key];
    entry.calibratedAlpha = alpha;
    entry.apronRadius = apronRadius;
    entry.emaLocalUs = localUs;
    entry.emaRemoteUs = remoteUs;
    entry.emaWallUs = wallUs;
    entry.lastAccess = std::chrono::steady_clock::now();
    ++entry.updateCount;
    m_stats.entryCount = m_entries.size();
}

float ApronCache::recordFeedback(protocol::TaskType type,
                                 uint32_t width,
                                 uint32_t height,
                                 uint64_t bandwidthBps,
                                 double actualLocalUs,
                                 double actualRemoteUs,
                                 double actualWallUs,
                                 float learningRate) noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    ApronCacheKey key{type, width, height, quantizeBandwidth(bandwidthBps)};
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        ApronCacheEntry entry;
        entry.calibratedAlpha = 0.50f;
        entry.emaLocalUs = actualLocalUs;
        entry.emaRemoteUs = actualRemoteUs;
        entry.emaWallUs = actualWallUs;
        entry.lastAccess = std::chrono::steady_clock::now();
        entry.updateCount = 1;
        m_entries[key] = entry;
        m_stats.entryCount = m_entries.size();
        it = m_entries.find(key);
    }

    auto& entry = it->second;
    entry.emaLocalUs  = (entry.emaLocalUs <= 0.0)  ? actualLocalUs  : (0.2 * actualLocalUs  + 0.8 * entry.emaLocalUs);
    entry.emaRemoteUs = (entry.emaRemoteUs <= 0.0) ? actualRemoteUs : (0.2 * actualRemoteUs + 0.8 * entry.emaRemoteUs);
    entry.emaWallUs   = (entry.emaWallUs <= 0.0)   ? actualWallUs   : (0.2 * actualWallUs   + 0.8 * entry.emaWallUs);

    double totalTime = entry.emaLocalUs + entry.emaRemoteUs;
    if (totalTime > 0.0) {
        // Load imbalance ratio: (local - remote) / total
        double imbalance = (entry.emaLocalUs - entry.emaRemoteUs) / totalTime;
        float delta = static_cast<float>(imbalance * learningRate);
        delta = std::clamp(delta, -0.10f, 0.10f);
        // If local took longer (imbalance > 0), give local fewer rows (decrease alpha)
        // If remote took longer (imbalance < 0), give local more rows (increase alpha)
        entry.calibratedAlpha = std::clamp(entry.calibratedAlpha - delta, 0.05f, 0.95f);
    }

    ++entry.updateCount;
    entry.lastAccess = std::chrono::steady_clock::now();
    ++m_stats.updates;
    return entry.calibratedAlpha;
}

void ApronCache::clear() noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    m_entries.clear();
    m_stats.entryCount = 0;
}

size_t ApronCache::size() const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_entries.size();
}

ApronCacheStats ApronCache::stats() const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    m_stats.entryCount = m_entries.size();
    return m_stats;
}

void ApronCache::resetStats() noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    m_stats = ApronCacheStats{};
    m_stats.entryCount = m_entries.size();
}

// ─── WorkloadPartitioner Methods ─────────────────────────────────────────────

uint32_t WorkloadPartitioner::getApronRadiusForTask(protocol::TaskType type, uint32_t intensity) noexcept {
    switch (type) {
    case protocol::TaskType::Blur: {
        uint32_t r = intensity & 0xFFu;
        return (r > 0) ? r : 9u;
    }
    case protocol::TaskType::EdgeDetection:
        return 1u;
    default:
        return 9u;
    }
}

ApronCache& WorkloadPartitioner::globalApronCache() noexcept {
    static ApronCache s_cache;
    return s_cache;
}

PartitionDecision
WorkloadPartitioner::calculateOptimalDecision(uint32_t width,
                                             uint32_t height,
                                             double localGpuTotalUs,
                                             double remoteGpuTotalUs,
                                             uint64_t linkBandwidthBps,
                                             uint64_t baseRttUs,
                                             float compressionRatio,
                                             double splitOverheadUs,
                                             double stitchOverheadUs,
                                             float compressionRatioRecv,
                                             uint32_t apronRadius,
                                             double compressCpuUsPerByte,
                                             double decompressCpuUsPerByte,
                                             protocol::TaskType type,
                                             ApronCache* cache) noexcept {
    PartitionDecision decision;
    if (width == 0 || height == 0 || localGpuTotalUs <= 0.0) {
        decision.splitRatio = 0.50f;
        decision.rationale = "Invalid parameters, default 50/50";
        return decision;
    }

    if (apronRadius == 0) {
        apronRadius = getApronRadiusForTask(type);
    }

    if (linkBandwidthBps == 0) linkBandwidthBps = 12'500'000;
    if (compressionRatio <= 0.05f) compressionRatio = 1.0f;
    if (compressionRatioRecv <= 0.05f) compressionRatioRecv = compressionRatio;
    decision.effectiveCompressionRatio = compressionRatio;
    decision.effectiveCompressionRatioRecv = compressionRatioRecv;
    decision.predictedSingleGpuUs = localGpuTotalUs;

    // Check ApronCache for pre-calibrated split ratio
    std::optional<float> cachedAlpha;
    if (cache) {
        cachedAlpha = cache->lookupAlpha(type, width, height, linkBandwidthBps);
    }

    const double totalPixels = static_cast<double>(width) * height;
    const double tLocalPx = localGpuTotalUs / totalPixels;

    // Estimate remote GPU compute rate per pixel
    const double tRemoteGpuPx = (remoteGpuTotalUs > 0.0)
                                    ? (remoteGpuTotalUs / totalPixels)
                                    : (tLocalPx * 1.25);

    // Initialize baseline (100% local execution)
    float bestF = 1.0f;
    double minPredictedWallUs = localGpuTotalUs;
    PartitionDecision bestCand;
    bestCand.splitRatio = 1.0f;
    bestCand.bestCandidateRatio = 0.50f;
    bestCand.predictedLocalUs = localGpuTotalUs;
    bestCand.predictedRemoteUs = 0.0;
    bestCand.predictedLocalBranchUs = localGpuTotalUs;
    bestCand.predictedRemoteBranchUs = 0.0;
    bestCand.predictedWallClockUs = localGpuTotalUs;
    bestCand.predictedSingleGpuUs = localGpuTotalUs;
    bestCand.isLocalFallback = true;

    // Evaluate candidate split ratios f from 0.05 to 0.95 in fine steps of 0.01
    // f represents the fraction of core height allocated to local GPU
    for (int step = 5; step <= 95; ++step) {
        float f = step * 0.01f;
        uint32_t localCoreH = static_cast<uint32_t>(std::round(static_cast<float>(height) * f));
        if (localCoreH < 1 || localCoreH >= height) continue;
        uint32_t remoteCoreH = height - localCoreH;

        // Accurate apron accounting: each branch processes its core rows plus halo apron
        uint32_t localInputH  = localCoreH + std::min(apronRadius, remoteCoreH);
        uint32_t remoteInputH = remoteCoreH + std::min(apronRadius, localCoreH);

        double localPixels  = static_cast<double>(width) * localInputH;
        double remotePixels = static_cast<double>(width) * remoteInputH;

        // 1. Local branch finish time
        double predLocal = localPixels * tLocalPx;

        // 2. Remote branch finish time (compression, transfer, compute, readback, decompression)
        double remoteInBytes  = remotePixels * 4.0;
        double remoteOutBytes = remotePixels * 4.0;

        // Uncompressed transfer wire time
        double uncompWireInUs  = (remoteInBytes  / static_cast<double>(linkBandwidthBps)) * 1'000'000.0;
        double uncompWireOutUs = (remoteOutBytes / static_cast<double>(linkBandwidthBps)) * 1'000'000.0;

        // Compressed transfer wire time + Software LZ4 CPU codec time
        double compWireInUs  = ((remoteInBytes / std::max(1.0f, compressionRatio)) / static_cast<double>(linkBandwidthBps)) * 1'000'000.0;
        double compCpuInUs   = remoteInBytes * compressCpuUsPerByte;

        double compWireOutUs = ((remoteOutBytes / std::max(1.0f, compressionRatioRecv)) / static_cast<double>(linkBandwidthBps)) * 1'000'000.0;
        double decompCpuOutUs = remoteOutBytes * decompressCpuUsPerByte;

        // Dynamic cost-aware compression decision: only compress if CPU codec time + compressed wire < uncompressed wire
        bool sendUsesComp = (compWireInUs + compCpuInUs < uncompWireInUs);
        bool recvUsesComp = (compWireOutUs + decompCpuOutUs < uncompWireOutUs);

        double wireSendUs = sendUsesComp ? compWireInUs : uncompWireInUs;
        double compUs     = sendUsesComp ? compCpuInUs  : 0.0;

        double wireRecvUs = recvUsesComp ? compWireOutUs : uncompWireOutUs;
        double decompUs   = recvUsesComp ? decompCpuOutUs : 0.0;

        double remoteComputeUs = remotePixels * tRemoteGpuPx;
        double rttUs = static_cast<double>(baseRttUs);

        double predRemote = compUs + wireSendUs + (rttUs * 0.5) + remoteComputeUs + (rttUs * 0.5) + wireRecvUs + decompUs;

        // 3. Synchronization & stitching
        double branchFinishUs = std::max(predLocal, predRemote);
        double stitchCost = splitOverheadUs + stitchOverheadUs;
        double predWallUs = branchFinishUs + stitchCost;

        if (step == 5 || predWallUs < minPredictedWallUs) {
            if (predWallUs < minPredictedWallUs) {
                minPredictedWallUs = predWallUs;
                bestF = f;
            }
            bestCand.bestCandidateRatio = f;
            bestCand.predictedLocalUs = predLocal;
            bestCand.predictedRemoteUs = predRemote;
            bestCand.predictedLocalBranchUs = predLocal;
            bestCand.predictedRemoteBranchUs = predRemote;
            bestCand.predictedWireSendUs = wireSendUs;
            bestCand.predictedWireRecvUs = wireRecvUs;
            bestCand.predictedCompressUs = compUs;
            bestCand.predictedDecompressUs = decompUs;
            bestCand.predictedWorkerGpuUs = remoteComputeUs;
            bestCand.predictedStitchUs = stitchCost;
            bestCand.predictedWallClockUs = predWallUs;
            bestCand.predictedCompressionHelps = (sendUsesComp || recvUsesComp);
        }
    }

    // If cache contained a calibrated alpha that beats or refines the analytical split,
    // factor in calibrated convergence
    if (cachedAlpha.has_value() && bestF < 0.99f) {
        float calAlpha = cachedAlpha.value();
        // Blend analytical bestF with empirical cachedAlpha (70% empirical calibrated, 30% analytical)
        bestF = 0.70f * calAlpha + 0.30f * bestF;
    }

    // Check if optimal candidate is faster than fastest single GPU by at least 2%
    if (bestF < 0.99f && minPredictedWallUs < localGpuTotalUs * 0.98) {
        decision = bestCand;
        decision.splitRatio = bestF;
        decision.isLocalFallback = false;
        decision.predictedSpeedupVsBestGpu = localGpuTotalUs / minPredictedWallUs;
        decision.rationale = std::format(
            "Cooperative split selected ({:.1f}% Local, {:.1f}% Remote): predicted wall-clock {:.2f} ms vs {:.2f} ms local ({:.2f}x speedup){}.",
            decision.splitRatio * 100.0f, (1.0f - decision.splitRatio) * 100.0f,
            minPredictedWallUs / 1000.0, localGpuTotalUs / 1000.0, decision.predictedSpeedupVsBestGpu,
            cachedAlpha.has_value() ? " [Calibrated from ApronCache]" : "");

        if (cache) {
            cache->put(type, width, height, linkBandwidthBps, decision.splitRatio, apronRadius,
                       decision.predictedLocalUs, decision.predictedRemoteUs, decision.predictedWallClockUs);
        }
    } else {
        decision = bestCand;
        decision.splitRatio = 1.0f;
        decision.predictedLocalUs = localGpuTotalUs;
        decision.predictedRemoteUs = 0.0;
        decision.predictedWallClockUs = localGpuTotalUs;
        decision.isLocalFallback = true;
        decision.predictedSpeedupVsBestGpu = 1.0;
        decision.rationale = std::format(
            "100% Local Fallback selected: min predicted distributed wall-clock ({:.2f} ms at {:.1f}% split) exceeds local baseline ({:.2f} ms) due to network transfer and sync overhead.",
            minPredictedWallUs / 1000.0, bestCand.bestCandidateRatio * 100.0f, localGpuTotalUs / 1000.0);
    }

    return decision;
}

float WorkloadPartitioner::calculateOptimalSplitRatio(uint32_t width,
                                                      uint32_t height,
                                                      double localGpuTotalUs,
                                                      double remoteGpuTotalUs,
                                                      uint64_t linkBandwidthBps,
                                                      uint64_t baseRttUs,
                                                      float compressionRatio,
                                                      uint32_t apronRadius) noexcept {
    if (width == 0 || height == 0 || localGpuTotalUs <= 0.0) {
        return 0.50f;
    }

    if (apronRadius == 0) apronRadius = 9;
    if (linkBandwidthBps == 0) linkBandwidthBps = 12'500'000;
    if (compressionRatio <= 0.05f) compressionRatio = 1.0f;

    double totalPixels = static_cast<double>(width) * height;
    double tLocalPx = localGpuTotalUs / totalPixels;

    double tRemotePx = (remoteGpuTotalUs > 0.0)
                           ? (remoteGpuTotalUs / totalPixels)
                           : (tLocalPx * 1.25); // conservative estimate if remote not yet sampled

    // Network transfer time in microseconds per pixel (round-trip: RGBA8 4 bytes in + 4 bytes out = 8 bytes)
    double bytesPerPixel = 8.0 / static_cast<double>(compressionRatio);
    double rTransferPx = (bytesPerPixel / static_cast<double>(linkBandwidthBps)) * 1'000'000.0;

    double rttPerPixel = static_cast<double>(baseRttUs) / totalPixels;

    // Optimal balance: f* = (tRemotePx + rTransferPx + rttPerPixel) / (tLocalPx + tRemotePx + rTransferPx)
    double numerator = tRemotePx + rTransferPx + rttPerPixel;
    double denominator = tLocalPx + tRemotePx + rTransferPx;

    if (denominator <= 0.0) return 0.50f;

    double f = numerator / denominator;
    return static_cast<float>(std::clamp(f, 0.05, 0.95));
}

std::pair<SpatialPartition, SpatialPartition>
WorkloadPartitioner::createSlabPartitions(const protocol::TaskRequest& fullReq,
                                          float splitRatio,
                                          uint32_t apronRadius) {
    const uint32_t fullW = fullReq.width;
    const uint32_t fullH = fullReq.height;

    if (apronRadius == 0) {
        apronRadius = getApronRadiusForTask(fullReq.type);
    }

    splitRatio = std::clamp(splitRatio, 0.05f, 0.95f);
    uint32_t localCoreH = static_cast<uint32_t>(std::round(static_cast<float>(fullH) * splitRatio));
    localCoreH = std::clamp(localCoreH, 1u, fullH - 1u);
    const uint32_t remoteCoreH = fullH - localCoreH;

    const size_t rowBytes = static_cast<size_t>(fullW) * 4;

    // ─── Partition 0: Top Slab (Host RTX 3050) ────────────────────────────────
    SpatialPartition p0;
    p0.partitionIndex = 0;
    p0.fullWidth      = fullW;
    p0.fullHeight     = fullH;
    p0.coreY          = 0;
    p0.coreHeight     = localCoreH;
    p0.apronTop       = 0; // Touches image top border
    p0.apronBottom    = std::min(apronRadius, remoteCoreH);
    p0.inputY         = 0;
    p0.inputHeight    = localCoreH + p0.apronBottom;

    p0.request.taskId = fullReq.taskId + 1;
    p0.request.type   = fullReq.type;
    p0.request.width  = fullW;
    p0.request.height = p0.inputHeight;

    size_t p0Bytes = static_cast<size_t>(p0.inputHeight) * rowBytes;
    if (fullReq.input.size() >= p0Bytes) {
        p0.request.input.assign(fullReq.input.begin(), fullReq.input.begin() + p0Bytes);
    }

    // ─── Partition 1: Bottom Slab (Worker GTX 1650 Ti) ────────────────────────
    SpatialPartition p1;
    p1.partitionIndex = 1;
    p1.fullWidth      = fullW;
    p1.fullHeight     = fullH;
    p1.coreY          = localCoreH;
    p1.coreHeight     = remoteCoreH;
    p1.apronTop       = std::min(apronRadius, localCoreH);
    p1.apronBottom    = 0; // Touches image bottom border
    p1.inputY         = localCoreH - p1.apronTop;
    p1.inputHeight    = remoteCoreH + p1.apronTop;

    p1.request.taskId = fullReq.taskId + 2;
    p1.request.type   = fullReq.type;
    p1.request.width  = fullW;
    p1.request.height = p1.inputHeight;

    size_t p1StartByte = static_cast<size_t>(p1.inputY) * rowBytes;
    size_t p1Bytes     = static_cast<size_t>(p1.inputHeight) * rowBytes;
    if (fullReq.input.size() >= p1StartByte + p1Bytes) {
        p1.request.input.assign(fullReq.input.begin() + p1StartByte,
                                fullReq.input.begin() + p1StartByte + p1Bytes);
    }

    return {std::move(p0), std::move(p1)};
}

std::vector<std::byte>
WorkloadPartitioner::stitchSlabPartitions(const SpatialPartition& part0,
                                         const protocol::TaskResult& res0,
                                         const SpatialPartition& part1,
                                         const protocol::TaskResult& res1) {
    const uint32_t fullW = part0.fullWidth;
    const uint32_t fullH = part0.fullHeight;
    const size_t   rowBytes = static_cast<size_t>(fullW) * 4;
    const size_t   totalBytes = rowBytes * fullH;

    std::vector<std::byte> stitched(totalBytes);

    // 1. Copy Partition 0 core rows (rows 0 to part0.coreHeight - 1)
    size_t p0CoreBytes = static_cast<size_t>(part0.coreHeight) * rowBytes;
    if (res0.output.size() >= p0CoreBytes) {
        std::memcpy(stitched.data(), res0.output.data(), p0CoreBytes);
    }

    // 2. Copy Partition 1 core rows (skip part1.apronTop rows from res1.output)
    size_t p1SrcOffset = static_cast<size_t>(part1.apronTop) * rowBytes;
    size_t p1DstOffset = static_cast<size_t>(part1.coreY) * rowBytes;
    size_t p1CoreBytes = static_cast<size_t>(part1.coreHeight) * rowBytes;

    if (res1.output.size() >= p1SrcOffset + p1CoreBytes &&
        stitched.size() >= p1DstOffset + p1CoreBytes) {
        std::memcpy(stitched.data() + p1DstOffset,
                    res1.output.data() + p1SrcOffset,
                    p1CoreBytes);
    }

    return stitched;
}

WorkloadPartitioner::PartitionResult
WorkloadPartitioner::executeParallelSlabs(const protocol::TaskRequest& fullReq,
                                          LocalExecFn localFn,
                                          RemoteExecFn remoteFn,
                                          float splitRatio,
                                          uint32_t apronRadius,
                                          ApronCache* cache,
                                          uint64_t linkBandwidthBps) {
    PartitionResult res;
    auto wallStart = std::chrono::steady_clock::now();

    if (apronRadius == 0) {
        apronRadius = getApronRadiusForTask(fullReq.type);
    }

    // Local-only fallback (Priority 2)
    if (splitRatio >= 0.999f) {
        res.timing.isLocalFallback = true;
        auto localStart = std::chrono::steady_clock::now();
        auto localRes = localFn(fullReq);
        auto localEnd = std::chrono::steady_clock::now();
        res.timing.localUploadUs   = localRes.uploadUs;
        res.timing.localGpuUs      = localRes.gpuUs;
        res.timing.localReadbackUs = localRes.readbackUs;
        res.timing.localTotalUs    = localRes.totalUs;
        res.timing.wallClockUs     = std::chrono::duration_cast<std::chrono::microseconds>(localEnd - localStart).count();
        res.output                 = std::move(localRes.output);
        res.success                = (localRes.status == protocol::TaskStatus::Ok);
        return res;
    }

    // 1. Split into halo slabs
    auto splitStart = std::chrono::steady_clock::now();
    auto [part0, part1] = createSlabPartitions(fullReq, splitRatio, apronRadius);
    auto splitEnd = std::chrono::steady_clock::now();
    res.timing.splitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             splitEnd - splitStart).count();

    // 2. Dispatch Local and Remote concurrently
    auto futLocal = std::async(std::launch::async, [&]() {
        return localFn(part0.request);
    });

    auto netStart = std::chrono::steady_clock::now();
    auto remoteRes = remoteFn(part1.request);
    auto netEnd   = std::chrono::steady_clock::now();

    auto localRes = futLocal.get();

    // 3. Timing accounting
    uint64_t roundTripUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               netEnd - netStart).count();

    res.timing.localUploadUs   = localRes.uploadUs;
    res.timing.localGpuUs      = localRes.gpuUs;
    res.timing.localReadbackUs = localRes.readbackUs;
    res.timing.localTotalUs    = localRes.totalUs;

    res.timing.remoteUploadUs   = remoteRes.uploadUs;
    res.timing.remoteGpuUs      = remoteRes.gpuUs;
    res.timing.remoteReadbackUs = remoteRes.readbackUs;
    res.timing.remoteTotalUs    = roundTripUs;

    uint64_t netOverheadUs = (roundTripUs > remoteRes.totalUs)
                                 ? (roundTripUs - remoteRes.totalUs) : 0;
    res.timing.remoteNetworkSendUs   = netOverheadUs / 2;
    res.timing.remoteNetworkReturnUs = netOverheadUs / 2;

    if (localRes.status != protocol::TaskStatus::Ok ||
        remoteRes.status != protocol::TaskStatus::Ok) {
        res.success = false;
        return res;
    }

    // 4. Stitch output frames
    auto stitchStart = std::chrono::steady_clock::now();
    res.output = stitchSlabPartitions(part0, localRes, part1, remoteRes);
    auto stitchEnd = std::chrono::steady_clock::now();

    res.timing.stitchUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              stitchEnd - stitchStart).count();

    auto wallEnd = std::chrono::steady_clock::now();
    res.timing.wallClockUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 wallEnd - wallStart).count();

    res.success = true;

    // Automatically record runtime feedback to calibrated ApronCache
    if (cache) {
        cache->recordFeedback(fullReq.type, fullReq.width, fullReq.height, linkBandwidthBps,
                             static_cast<double>(res.timing.localTotalUs),
                             static_cast<double>(res.timing.remoteTotalUs),
                             static_cast<double>(res.timing.wallClockUs));
    }

    return res;
}

} // namespace dgpu::host
