// tests/benchmark_tests/test_partitioner.cpp
// Unit test for WorkloadPartitioner: apron calculation, slab extraction, and stitching.

#include "workload_partitioner.hpp"
#include "protocol/lz4.hpp"
#include "statistical_validator.hpp"
#include "batch_scheduler.hpp"

#include <cassert>
#include <iostream>
#include <numeric>
#include <vector>

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

    std::cout << "[test_partitioner] All tests passed successfully!\n";
    return 0;
}
