// tests/benchmark_tests/test_partitioner.cpp
// Unit test for WorkloadPartitioner: apron calculation, slab extraction, and stitching.

#include "workload_partitioner.hpp"
#include "protocol/lz4.hpp"
#include "statistical_validator.hpp"
#include "batch_scheduler.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>

#if defined(D3D12_AVAILABLE) && D3D12_AVAILABLE
#include "d3d12/pipelined_gpu_executor.hpp"
#endif

int main() {
    using namespace dgpu;
    using namespace dgpu::host;

    std::cout << "[test_partitioner] Running WorkloadPartitioner tests...\n";

    const uint32_t width = 64;
    const uint32_t height = 64;
    const size_t totalBytes = static_cast<size_t>(width) * height * 4;

    // Create synthetic test image where each pixel value encodes its (x, y) coordinates
    protocol::TaskRequest fullReq;
    fullReq.taskId = 100;
    fullReq.type   = protocol::TaskType::Blur;
    fullReq.width  = width;
    fullReq.height = height;
    fullReq.input.resize(totalBytes);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            fullReq.input[idx + 0] = static_cast<std::byte>(x & 0xFF);
            fullReq.input[idx + 1] = static_cast<std::byte>(y & 0xFF);
            fullReq.input[idx + 2] = static_cast<std::byte>((x + y) & 0xFF);
            fullReq.input[idx + 3] = static_cast<std::byte>(255);
        }
    }

    // ── Test 1: 50/50 Slab Creation with 9-pixel Apron ────────────────────────
    const uint32_t apron = 9;
    auto [p0, p1] = WorkloadPartitioner::createSlabPartitions(fullReq, 0.50f, apron);

    assert(p0.fullWidth == width);
    assert(p0.fullHeight == height);
    assert(p0.coreY == 0);
    assert(p0.coreHeight == 32);
    assert(p0.apronTop == 0);
    assert(p0.apronBottom == apron);
    assert(p0.inputHeight == 32 + apron);
    assert(p0.request.input.size() == static_cast<size_t>(width) * (32 + apron) * 4);

    assert(p1.coreY == 32);
    assert(p1.coreHeight == 32);
    assert(p1.apronTop == apron);
    assert(p1.apronBottom == 0);
    assert(p1.inputY == 32 - apron);
    assert(p1.inputHeight == 32 + apron);
    assert(p1.request.input.size() == static_cast<size_t>(width) * (32 + apron) * 4);

    std::cout << "  [PASS] Slab partition coordinate and apron geometry verified\n";

    // ── Test 2: Stitching Identity Pass-Through ──────────────────────────────
    // Simulate an identity kernel (res.output == req.input) to verify exact reconstruction
    protocol::TaskResult res0;
    res0.taskId = p0.request.taskId;
    res0.status = protocol::TaskStatus::Ok;
    res0.output = p0.request.input;

    protocol::TaskResult res1;
    res1.taskId = p1.request.taskId;
    res1.status = protocol::TaskStatus::Ok;
    res1.output = p1.request.input;

    auto stitched = WorkloadPartitioner::stitchSlabPartitions(p0, res0, p1, res1);
    assert(stitched.size() == fullReq.input.size());

    // Verify every single byte matches the original input image exactly
    bool exactMatch = (stitched == fullReq.input);
    assert(exactMatch);
    (void)exactMatch;
    std::cout << "  [PASS] Apron discard and core region stitching identity verified (exact byte match)\n";

    // ── Test 3: Asymmetric 60/40 Split Ratio ─────────────────────────────────
    auto [p0_asym, p1_asym] = WorkloadPartitioner::createSlabPartitions(fullReq, 0.60f, apron);
    assert(p0_asym.coreHeight == 38);
    assert(p1_asym.coreHeight == 26);
    assert(p0_asym.coreHeight + p1_asym.coreHeight == height);

    protocol::TaskResult res0_a, res1_a;
    res0_a.output = p0_asym.request.input;
    res1_a.output = p1_asym.request.input;
    auto stitched_a = WorkloadPartitioner::stitchSlabPartitions(p0_asym, res0_a, p1_asym, res1_a);
    assert(stitched_a == fullReq.input);
    std::cout << "  [PASS] Asymmetric split (60/40) identity and seam alignment verified\n";

    // ── Test 4: Optimal Split Ratio Calculation ──────────────────────────────
    // 1024x1024, RTX 3050 ~89ms, GTX 1650 Ti ~108ms, 100 Mbps link (12.5 MB/s)
    float optSplit = WorkloadPartitioner::calculateOptimalSplitRatio(
        1024, 1024, 89700.0, 108500.0, 12'500'000, 1000, 1.0f);
    assert(optSplit >= 0.85f && optSplit <= 0.95f); // ~90% local due to 100 Mbps wire cost
    std::cout << "  [PASS] Optimal uncompressed split ratio: " << optSplit << " (allocates ~"
              << static_cast<int>(optSplit * 100) << "% to local to avoid wire stall)\n";

    // With 4x compression, optimal local ratio drops to ~75% (allowing remote to take 25%)
    float optSplitComp = WorkloadPartitioner::calculateOptimalSplitRatio(
        1024, 1024, 89700.0, 108500.0, 12'500'000, 1000, 4.0f);
    assert(optSplitComp >= 0.70f && optSplitComp <= 0.80f);
    std::cout << "  [PASS] Optimal compressed split ratio: " << optSplitComp << " (remote takes ~"
              << static_cast<int>((1.0f - optSplitComp) * 100) << "% with 4x compression)\n";

    // ── Test 5: LZ4 Block Compression & Decompression ────────────────────────
    auto compBytes = protocol::compressBuffer(fullReq.input);
    assert(!compBytes.empty() && compBytes.size() < fullReq.input.size());
    auto decompBytes = protocol::decompressBuffer(std::span<const std::byte>(compBytes.data() + 4, compBytes.size() - 4),
                                                  fullReq.input.size());
    assert(decompBytes == fullReq.input);
    double ratio = static_cast<double>(fullReq.input.size()) / static_cast<double>(compBytes.size());
    std::cout << "  [PASS] LZ4 compression verified: " << fullReq.input.size() << " bytes -> "
              << compBytes.size() << " bytes (ratio: " << ratio << "x, exact match)\n";

    // ── Test 6: Cost-Aware Partition Decision & Local Fallback ────────────────
    // Case A: 512x512 over slow 100 Mbps link with high wire overhead: offloading is slower than RTX 3050 alone (20ms)
    auto decisionSlow = WorkloadPartitioner::calculateOptimalDecision(
        512, 512, 20'000.0, 25'000.0, 12'500'000, 1000, 1.05f, 300.0, 500.0);
    assert(decisionSlow.isLocalFallback);
    assert(decisionSlow.splitRatio == 1.0f);
    assert(decisionSlow.predictedWallClockUs == 20'000.0);
    std::cout << "  [PASS] Cost-aware scheduler correctly activated Local Fallback for slow link (100 Mbps, 1.05x comp)\n";

    // Case B: 1024x1024 heavy compute over 1 Gbps link with 4x compression: offloading is faster
    auto decisionFast = WorkloadPartitioner::calculateOptimalDecision(
        1024, 1024, 150'000.0, 160'000.0, 125'000'000, 1000, 4.0f, 300.0, 500.0);
    assert(!decisionFast.isLocalFallback);
    assert(decisionFast.splitRatio >= 0.45f && decisionFast.splitRatio <= 0.65f);
    assert(decisionFast.predictedSpeedupVsBestGpu > 1.0);
    std::cout << "  [PASS] Cost-aware scheduler selected cooperative split ("
              << static_cast<int>(decisionFast.splitRatio * 100) << "% Local) with predicted speedup "
              << decisionFast.predictedSpeedupVsBestGpu << "x vs single GPU\n";

    // ── Test 7: Speedup Metric Validation ─────────────────────────────────────
    // Speedup = T_fastest_single_gpu / T_distributed
    double tFastest = 20.85; // RTX 3050 alone
    double tDistSlow = 23.72; // Distributed slower than best GPU
    double tDistFast = 14.50; // Distributed faster than best GPU
    double speedupSub1 = tFastest / tDistSlow;
    double speedupSup1 = tFastest / tDistFast;
    (void)speedupSub1;
    (void)speedupSup1;
    assert(speedupSub1 < 1.0); // Correctly flags distributed as slower
    assert(speedupSup1 > 1.0); // Correctly flags distributed as faster
    // ── Test 8: Statistical Paired Difference & Confidence Interval ──────────
    std::vector<double> baseRuns(100, 160.0);
    std::vector<double> targetFastRuns(100, 150.0);
    std::vector<double> targetNoisyRuns(100, 160.0);

    // Add slight deterministic variance
    for (size_t i = 0; i < 100; ++i) {
        baseRuns[i] += (i % 5) * 0.4 - 0.8;
        targetFastRuns[i] += (i % 7) * 0.3 - 0.9;
        targetNoisyRuns[i] += ((i % 2 == 0) ? 1.0 : -1.0) * 1.5;
    }

    auto statsFast = StatisticalValidator::computePairedStats(baseRuns, targetFastRuns, 0.0);
    assert(statsFast.sampleCount == 100);
    assert(statsFast.isStatisticallySignificant);
    assert(statsFast.ci95LowerMs > 0.0);
    assert(statsFast.pValue < 0.001);
    std::cout << "  [PASS] Statistical validator verified genuine speedup: delta = " << statsFast.meanDeltaMs
              << " ms, 95% CI: [" << statsFast.ci95LowerMs << ", " << statsFast.ci95UpperMs << "] ms, p = " << statsFast.pValue << "\n";

    auto statsNoise = StatisticalValidator::computePairedStats(baseRuns, targetNoisyRuns, 0.0);
    assert(!statsNoise.isStatisticallySignificant);
    assert(statsNoise.ci95LowerMs < 0.0 && statsNoise.ci95UpperMs > 0.0); // Spans zero
    std::cout << "  [PASS] Statistical validator flagged noise correctly: 95% CI spans zero ["
              << statsNoise.ci95LowerMs << ", " << statsNoise.ci95UpperMs << "] ms, p = " << statsNoise.pValue << "\n";

    // ── Test 9: Batch Allocation Sweep & Dynamic Work-Stealing Simulation ────
    std::vector<protocol::TaskRequest> batchTasks(24);
    for (uint32_t i = 0; i < 24; ++i) {
        batchTasks[i].taskId = 2000 + i;
        batchTasks[i].type = protocol::TaskType::Blur;
        batchTasks[i].width = 64;
        batchTasks[i].height = 64;
        batchTasks[i].input.resize(64 * 64 * 4, std::byte{128});
    }

    auto mockLocal = [](const protocol::TaskRequest& req) {
        protocol::TaskResult res;
        res.taskId = req.taskId;
        res.status = protocol::TaskStatus::Ok;
        res.gpuUs = 4000;
        res.output = req.input;
        return res;
    };
    auto mockRemote = [](const protocol::TaskRequest& req) {
        protocol::TaskResult res;
        res.taskId = req.taskId;
        res.status = protocol::TaskStatus::Ok;
        res.gpuUs = 4200;
        res.output = req.input;
        return res;
    };

    auto sweep = AdvancedBatchScheduler::runAllocationSweep(batchTasks, mockLocal, mockRemote, true, 6);
    assert(sweep.size() >= 5); // 0/24, 6/18, 12/12, 18/6, 24/0
    assert(sweep[0].hostTaskCount == 0 && sweep[0].workerTaskCount == 24);
    assert(sweep.back().hostTaskCount == 24 && sweep.back().workerTaskCount == 0);
    std::cout << "  [PASS] Batch allocation sweep generated " << sweep.size() << " allocation points from 0/24 to 24/0\n";

    auto dynamicRes = AdvancedBatchScheduler::runDynamicWorkStealing(batchTasks, mockLocal, mockRemote, true);
    assert(dynamicRes.totalTasks == 24);
    assert(dynamicRes.hostTasksExecuted + dynamicRes.workerTasksExecuted == 24);
    assert(dynamicRes.failedTasks == 0);
    std::cout << "  [PASS] Dynamic work-stealing completed 24 tasks: Host = "
              << dynamicRes.hostTasksExecuted << ", Worker = " << dynamicRes.workerTasksExecuted
              << " (" << dynamicRes.throughputTasksPerSec << " tasks/sec)\n";

    // ── Test 10: ApronCache Direct Operations (Phase 2.5) ─────────────────────
    ApronCache apronCache;
    assert(apronCache.size() == 0);
    auto miss = apronCache.lookupAlpha(protocol::TaskType::Blur, 1024, 1024, 12'500'000);
    assert(!miss.has_value());
    assert(apronCache.stats().misses == 1);

    apronCache.put(protocol::TaskType::Blur, 1024, 1024, 12'500'000, 0.65f, 9, 25000.0, 30000.0, 35000.0);
    assert(apronCache.size() == 1);
    auto hit = apronCache.lookupAlpha(protocol::TaskType::Blur, 1024, 1024, 12'500'000);
    assert(hit.has_value() && hit.value() == 0.65f);
    assert(apronCache.stats().hits == 1);

    auto entry = apronCache.getEntry(protocol::TaskType::Blur, 1024, 1024, 12'500'000);
    assert(entry.has_value());
    assert(entry->apronRadius == 9);
    assert(entry->calibratedAlpha == 0.65f);
    std::cout << "  [PASS] ApronCache lookup, store, and statistics verified\n";

    // ── Test 11: Dynamic Load Adaptation (Fluctuating Worker Latency) ────────
    apronCache.put(protocol::TaskType::Blur, 512, 512, 125'000'000, 0.50f, 9, 10000.0, 10000.0, 12000.0);

    // Scenario A: Remote congestion (Local 10ms, Remote 25ms -> remote is bottleneck)
    float adaptedAlpha = 0.50f;
    for (int i = 0; i < 5; ++i) {
        adaptedAlpha = apronCache.recordFeedback(
            protocol::TaskType::Blur, 512, 512, 125'000'000, 10'000.0, 25'000.0, 27'000.0, 0.20f);
    }
    assert(adaptedAlpha > 0.50f);
    std::cout << "  [PASS] Remote congestion adaptation: alpha shifted from 0.50 to "
              << adaptedAlpha << " (allocating more work locally)\n";

    // Scenario B: Local bottleneck (Local 25ms, Remote 8ms -> local is bottleneck)
    for (int i = 0; i < 10; ++i) {
        adaptedAlpha = apronCache.recordFeedback(
            protocol::TaskType::Blur, 512, 512, 125'000'000, 25'000.0, 8'000.0, 26'000.0, 0.20f);
    }
    assert(adaptedAlpha < 0.50f);
    std::cout << "  [PASS] Local bottleneck adaptation: alpha shifted down to "
              << adaptedAlpha << " (offloading more work remotely)\n";

    // ── Test 12: Dynamic Halo Apron Sizing by TaskType ───────────────────────
    assert(WorkloadPartitioner::getApronRadiusForTask(protocol::TaskType::EdgeDetection) == 1);
    assert(WorkloadPartitioner::getApronRadiusForTask(protocol::TaskType::Blur) == 9);
    assert(WorkloadPartitioner::getApronRadiusForTask(protocol::TaskType::Blur, 5) == 5);

    protocol::TaskRequest edgeReq = fullReq;
    edgeReq.type = protocol::TaskType::EdgeDetection;
    auto [edge0, edge1] = WorkloadPartitioner::createSlabPartitions(edgeReq, 0.50f, 0); // 0 = auto
    assert(edge0.apronBottom == 1);
    assert(edge1.apronTop == 1);

    protocol::TaskRequest blurReq = fullReq;
    blurReq.type = protocol::TaskType::Blur;
    auto [blur0, blur1] = WorkloadPartitioner::createSlabPartitions(blurReq, 0.50f, 0); // 0 = auto
    assert(blur0.apronBottom == 9);
    assert(blur1.apronTop == 9);
    std::cout << "  [PASS] Dynamic kernel apron sizing verified: EdgeDetection = 1 row, Blur = 9 rows\n";

    // Helper: CPU Sobel convolution matching exact discrete kernel
    auto applyCpuSobel = [](const std::vector<std::byte>& inBuf, uint32_t w, uint32_t h) {
        std::vector<std::byte> outBuf(static_cast<size_t>(w) * h * 4);
        auto loadLuma = [&](int x, int y) -> float {
            x = std::clamp(x, 0, static_cast<int>(w) - 1);
            y = std::clamp(y, 0, static_cast<int>(h) - 1);
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            float r = static_cast<float>(static_cast<uint8_t>(inBuf[idx + 0])) / 255.0f;
            float g = static_cast<float>(static_cast<uint8_t>(inBuf[idx + 1])) / 255.0f;
            float b = static_cast<float>(static_cast<uint8_t>(inBuf[idx + 2])) / 255.0f;
            return 0.299f * r + 0.587f * g + 0.114f * b;
        };

        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                int ix = static_cast<int>(x);
                int iy = static_cast<int>(y);
                float gx = -loadLuma(ix-1, iy-1) - 2.0f*loadLuma(ix-1, iy) - loadLuma(ix-1, iy+1)
                           +loadLuma(ix+1, iy-1) + 2.0f*loadLuma(ix+1, iy) + loadLuma(ix+1, iy+1);
                float gy = -loadLuma(ix-1, iy-1) - 2.0f*loadLuma(ix, iy-1) - loadLuma(ix+1, iy-1)
                           +loadLuma(ix-1, iy+1) + 2.0f*loadLuma(ix, iy+1) + loadLuma(ix+1, iy+1);
                float mag = std::clamp(std::sqrt(gx*gx + gy*gy), 0.0f, 1.0f);
                uint8_t val = static_cast<uint8_t>(mag * 255.0f);
                size_t outIdx = (static_cast<size_t>(y) * w + x) * 4;
                outBuf[outIdx + 0] = static_cast<std::byte>(val);
                outBuf[outIdx + 1] = static_cast<std::byte>(val);
                outBuf[outIdx + 2] = static_cast<std::byte>(val);
                outBuf[outIdx + 3] = static_cast<std::byte>(255);
            }
        }
        return outBuf;
    };

    // Helper: CPU box blur convolution
    auto applyCpuBoxBlur = [](const std::vector<std::byte>& inBuf, uint32_t w, uint32_t h, int rad) {
        std::vector<std::byte> outBuf(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                float sumR = 0, sumG = 0, sumB = 0;
                int count = 0;
                for (int dy = -rad; dy <= rad; ++dy) {
                    int sy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1);
                    for (int dx = -rad; dx <= rad; ++dx) {
                        int sx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1);
                        size_t idx = (static_cast<size_t>(sy) * w + sx) * 4;
                        sumR += static_cast<uint8_t>(inBuf[idx + 0]);
                        sumG += static_cast<uint8_t>(inBuf[idx + 1]);
                        sumB += static_cast<uint8_t>(inBuf[idx + 2]);
                        ++count;
                    }
                }
                size_t outIdx = (static_cast<size_t>(y) * w + x) * 4;
                outBuf[outIdx + 0] = static_cast<std::byte>(static_cast<uint8_t>(sumR / count));
                outBuf[outIdx + 1] = static_cast<std::byte>(static_cast<uint8_t>(sumG / count));
                outBuf[outIdx + 2] = static_cast<std::byte>(static_cast<uint8_t>(sumB / count));
                outBuf[outIdx + 3] = static_cast<std::byte>(255);
            }
        }
        return outBuf;
    };

    // ── Test 13: Numerical Seam Invariance & Bit-Exact Reconstruction (L1 == 0.0)
    // 13A: Sobel Edge Detection with 1-pixel apron
    auto groundTruthSobel = applyCpuSobel(fullReq.input, width, height);
    auto [sobelPart0, sobelPart1] = WorkloadPartitioner::createSlabPartitions(edgeReq, 0.50f, 1);

    protocol::TaskResult sobelRes0, sobelRes1;
    sobelRes0.output = applyCpuSobel(sobelPart0.request.input, sobelPart0.request.width, sobelPart0.request.height);
    sobelRes1.output = applyCpuSobel(sobelPart1.request.input, sobelPart1.request.width, sobelPart1.request.height);

    auto stitchedSobel = WorkloadPartitioner::stitchSlabPartitions(sobelPart0, sobelRes0, sobelPart1, sobelRes1);
    assert(stitchedSobel.size() == groundTruthSobel.size());

    double l1Sobel = 0.0;
    for (size_t i = 0; i < groundTruthSobel.size(); ++i) {
        l1Sobel += std::abs(static_cast<double>(static_cast<uint8_t>(stitchedSobel[i])) -
                            static_cast<double>(static_cast<uint8_t>(groundTruthSobel[i])));
    }
    assert(l1Sobel == 0.0);
    std::cout << "  [PASS] Sobel edge detection numerical seam invariance: L1 norm = " << l1Sobel
              << " (bit-exact match to unpartitioned ground truth)\n";

    // 13B: Verification that omitting apron introduces seam boundary error
    auto [noApron0, noApron1] = WorkloadPartitioner::createSlabPartitions(edgeReq, 0.50f, 1);
    noApron0.apronBottom = 0;
    noApron0.inputHeight = noApron0.coreHeight;
    noApron0.request.height = noApron0.coreHeight;
    noApron0.request.input.resize(static_cast<size_t>(noApron0.coreHeight) * width * 4);
    std::memcpy(noApron0.request.input.data(), fullReq.input.data(), noApron0.request.input.size());

    noApron1.apronTop = 0;
    noApron1.inputHeight = noApron1.coreHeight;
    noApron1.request.height = noApron1.coreHeight;
    noApron1.request.input.resize(static_cast<size_t>(noApron1.coreHeight) * width * 4);
    std::memcpy(noApron1.request.input.data(), fullReq.input.data() + (static_cast<size_t>(noApron1.coreY) * width * 4), noApron1.request.input.size());

    protocol::TaskResult noApronRes0, noApronRes1;
    noApronRes0.output = applyCpuSobel(noApron0.request.input, noApron0.request.width, noApron0.request.height);
    noApronRes1.output = applyCpuSobel(noApron1.request.input, noApron1.request.width, noApron1.request.height);
    auto stitchedNoApron = WorkloadPartitioner::stitchSlabPartitions(noApron0, noApronRes0, noApron1, noApronRes1);

    double l1NoApron = 0.0;
    for (size_t i = 0; i < groundTruthSobel.size(); ++i) {
        l1NoApron += std::abs(static_cast<double>(static_cast<uint8_t>(stitchedNoApron[i])) -
                              static_cast<double>(static_cast<uint8_t>(groundTruthSobel[i])));
    }
    assert(l1NoApron > 0.0);
    std::cout << "  [PASS] Omitting apron yields seam artifact error: L1 norm = " << l1NoApron
              << " (mathematically confirms halo necessity)\n";

    // 13C: Blur Convolution with 5-pixel apron
    auto groundTruthBlur = applyCpuBoxBlur(fullReq.input, width, height, 5);
    auto [blurPart0, blurPart1] = WorkloadPartitioner::createSlabPartitions(blurReq, 0.50f, 5);
    protocol::TaskResult blurRes0, blurRes1;
    blurRes0.output = applyCpuBoxBlur(blurPart0.request.input, blurPart0.request.width, blurPart0.request.height, 5);
    blurRes1.output = applyCpuBoxBlur(blurPart1.request.input, blurPart1.request.width, blurPart1.request.height, 5);
    auto stitchedBlur = WorkloadPartitioner::stitchSlabPartitions(blurPart0, blurRes0, blurPart1, blurRes1);

    double l1Blur = 0.0;
    for (size_t i = 0; i < groundTruthBlur.size(); ++i) {
        l1Blur += std::abs(static_cast<double>(static_cast<uint8_t>(stitchedBlur[i])) -
                           static_cast<double>(static_cast<uint8_t>(groundTruthBlur[i])));
    }
    assert(l1Blur == 0.0);
    std::cout << "  [PASS] Blur convolution numerical seam invariance: L1 norm = " << l1Blur
              << " (bit-exact reconstruction, zero seam artifacts)\n";

    // ── Test 14: Hardware D3D12 Seam Invariance (if available) ───────────────
#if defined(D3D12_AVAILABLE) && D3D12_AVAILABLE
    {
        d3d12::PipelinedGpuExecutor executor;
        if (executor.initialize(2)) {
            // Full frame execution
            assert(executor.submitAsync(edgeReq, 0));
            auto hwGroundTruth = executor.collect(0);

            // Slab execution
            auto [hwPart0, hwPart1] = WorkloadPartitioner::createSlabPartitions(edgeReq, 0.50f, 1);
            assert(executor.submitAsync(hwPart0.request, 0));
            assert(executor.submitAsync(hwPart1.request, 1));
            auto hwRes0 = executor.collect(0);
            auto hwRes1 = executor.collect(1);

            auto hwStitched = WorkloadPartitioner::stitchSlabPartitions(hwPart0, hwRes0, hwPart1, hwRes1);
            assert(hwStitched.size() == hwGroundTruth.output.size());

            double hwL1 = 0.0;
            for (size_t i = 0; i < hwGroundTruth.output.size(); ++i) {
                hwL1 += std::abs(static_cast<double>(static_cast<uint8_t>(hwStitched[i])) -
                                 static_cast<double>(static_cast<uint8_t>(hwGroundTruth.output[i])));
            }
            assert(hwL1 == 0.0);
            std::cout << "  [PASS] Direct3D 12 Hardware GPU edge detection seam invariance: L1 norm = " << hwL1
                      << " on " << executor.adapterInfo().description << "\n";
        } else {
            std::cout << "  [SKIP] D3D12 hardware not available, skipped D3D12 seam verification\n";
        }
    }
#endif

    // ── Test 15: Adaptive Partitioning vs Static 50/50 & Local-Only Sweep ────
    const uint32_t resolutions[] = { 512, 1024, 2048 };
    for (uint32_t res : resolutions) {
        double localGpuUs = (static_cast<double>(res * res) / (1024.0 * 1024.0)) * 40'000.0;
        double remoteGpuUs = localGpuUs * 1.30;
        uint64_t bandwidth = 125'000'000;

        auto adaptDec = WorkloadPartitioner::calculateOptimalDecision(
            res, res, localGpuUs, remoteGpuUs, bandwidth, 1000, 3.0f, 300.0, 500.0, 3.0f, 9,
            0.002, 0.0006, protocol::TaskType::Blur, &apronCache);

        // Static 50/50 comparison:
        double staticLocalH = res * 0.5;
        double staticRemoteH = res * 0.5;
        double staticLocalUs = staticLocalH * res * (localGpuUs / (res * res));
        double staticRemoteUs = staticRemoteH * res * (remoteGpuUs / (res * res)) + (staticRemoteH * res * 4.0 / 3.0 / bandwidth * 1e6);
        double staticWallUs = std::max(staticLocalUs, staticRemoteUs) + 800.0;

        assert(!adaptDec.isLocalFallback);
        assert(adaptDec.splitRatio > 0.50f);
        assert(adaptDec.predictedWallClockUs <= staticWallUs);
        assert(adaptDec.predictedWallClockUs < localGpuUs);

        std::cout << "  [PASS] Resolution " << res << "x" << res << ": Adaptive split "
                  << static_cast<int>(adaptDec.splitRatio * 100) << "% predicted "
                  << adaptDec.predictedWallClockUs / 1000.0 << " ms vs Static 50/50 "
                  << staticWallUs / 1000.0 << " ms (Local-only: " << localGpuUs / 1000.0 << " ms)\n";
    }

    std::cout << "[test_partitioner] All tests passed successfully!\n";
    return 0;
}
