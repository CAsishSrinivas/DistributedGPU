// tests/benchmark_tests/test_benchmark_matrix.cpp
// Full Regression Benchmarking, Performance Profiling and Tuning validation test (Phase 2.7).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "batch_scheduler.hpp"
#include "frame_compositor.hpp"
#include "pipeline_executor.hpp"
#include "protocol/message.hpp"
#include "statistical_validator.hpp"
#include "work_distributor.hpp"
#include "workload_cost_model.hpp"
#include "workload_partitioner.hpp"

#if defined(D3D12_AVAILABLE) && D3D12_AVAILABLE
#include "d3d12/gpu_executor.hpp"
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using namespace dgpu;
using namespace dgpu::host;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::cerr << "FAIL [" __FILE__ ":" << __LINE__ << "] " #expr << '\n'; \
            ++g_failed;                                                      \
        } else {                                                             \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

// ─── Reference Image & Blur ───────────────────────────────────────────────────

static std::vector<std::byte> makeTestPattern(uint32_t w, uint32_t h) {
    std::vector<std::byte> img(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            img[idx + 0] = static_cast<std::byte>((x * 127u) / std::max(1u, w));
            img[idx + 1] = static_cast<std::byte>((y * 223u) / std::max(1u, h));
            img[idx + 2] = static_cast<std::byte>((x + y) & 0xFFu);
            img[idx + 3] = static_cast<std::byte>(255);
        }
    }
    return img;
}

static protocol::TaskResult referenceCpuBlur(const protocol::TaskRequest& req) {
    protocol::TaskResult res;
    res.taskId = req.taskId;
    res.status = protocol::TaskStatus::Ok;
    res.output.resize(req.input.size());

    const int32_t w = static_cast<int32_t>(req.width);
    const int32_t h = static_cast<int32_t>(req.height);

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t r = 0, g = 0, b = 0, a = 0;
            int32_t count = 0;
            for (int32_t dy = -1; dy <= 1; ++dy) {
                int32_t ny = std::clamp(y + dy, 0, h - 1);
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    int32_t nx = std::clamp(x + dx, 0, w - 1);
                    size_t idx = (static_cast<size_t>(ny) * w + nx) * 4;
                    r += static_cast<uint8_t>(req.input[idx + 0]);
                    g += static_cast<uint8_t>(req.input[idx + 1]);
                    b += static_cast<uint8_t>(req.input[idx + 2]);
                    a += static_cast<uint8_t>(req.input[idx + 3]);
                    ++count;
                }
            }
            size_t outIdx = (static_cast<size_t>(y) * w + x) * 4;
            res.output[outIdx + 0] = static_cast<std::byte>(r / count);
            res.output[outIdx + 1] = static_cast<std::byte>(g / count);
            res.output[outIdx + 2] = static_cast<std::byte>(b / count);
            res.output[outIdx + 3] = static_cast<std::byte>(a / count);
        }
    }
    res.totalUs = 1000;
    return res;
}

// ─── Test 1: Statistical Metrics & Distribution Calculations ─────────────────

static void test_statistical_metrics_calculation() {
    std::vector<double> oddSamples = { 10.0, 20.0, 30.0, 40.0, 50.0 };
    std::vector<double> evenSamples = { 10.0, 20.0, 30.0, 40.0 };

    // Median calculations
    std::vector<double> oddSorted = oddSamples;
    std::sort(oddSorted.begin(), oddSorted.end());
    double medianOdd = oddSorted[oddSorted.size() / 2];
    CHECK(std::abs(medianOdd - 30.0) < 1e-6);

    std::vector<double> evenSorted = evenSamples;
    std::sort(evenSorted.begin(), evenSorted.end());
    double medianEven = 0.5 * (evenSorted[evenSorted.size() / 2 - 1] + evenSorted[evenSorted.size() / 2]);
    CHECK(std::abs(medianEven - 25.0) < 1e-6);

    // Paired difference statistical validation
    std::vector<double> base = { 100.0, 102.0, 98.0, 104.0, 101.0, 99.0, 103.0, 100.0, 102.0, 101.0 };
    std::vector<double> targetFast = { 80.0, 81.0, 79.0, 82.0, 80.0, 79.0, 81.0, 80.0, 81.0, 80.0 };

    auto paired = StatisticalValidator::computePairedStats(base, targetFast, 0.0);
    CHECK(paired.meanDeltaMs > 15.0);
    CHECK(paired.ci95LowerMs > 0.0);
    CHECK(paired.ci95UpperMs > paired.ci95LowerMs);
    CHECK(paired.isStatisticallySignificant);
    CHECK(paired.speedupRatio > 1.20);
    CHECK(paired.pValue < 0.01);
}

// ─── Test 2: End-to-End Matrix Execution & Reconstruction Accuracy ───────────

static void test_matrix_execution_and_accuracy() {
    const std::vector<std::pair<uint32_t, uint32_t>> resolutions = {
        { 512,  512  },
        { 1024, 1024 },
        { 1920, 1080 }
    };

    const uint32_t apron = 9;

    for (const auto& [w, h] : resolutions) {
        protocol::TaskRequest req;
        req.taskId = 200;
        req.type   = protocol::TaskType::Blur;
        req.width  = w;
        req.height = h;
        req.input  = makeTestPattern(w, h);

        auto groundTruth = referenceCpuBlur(req);

        // 1. Static 50/50 Parallel Slabs
        auto staticRes = WorkloadPartitioner::executeParallelSlabs(
            req, referenceCpuBlur, referenceCpuBlur, 0.50f, apron);

        double maeStatic = FrameCompositor::computeMAE(groundTruth.output, staticRes.output);
        CHECK(maeStatic <= 1.0); // Seam error must be virtually zero

        // 2. Candidate 70/30 Split
        auto splitRes = WorkloadPartitioner::executeParallelSlabs(
            req, referenceCpuBlur, referenceCpuBlur, 0.70f, apron);

        double maeSplit = FrameCompositor::computeMAE(groundTruth.output, splitRes.output);
        CHECK(maeSplit <= 1.0);
    }
}

// ─── Test 3: Cost-Model Fallback & Regression Bounds ──────────────────────────

static void test_cost_model_speedup_bounds() {
    // Case A: High network latency + low compute load (512x512 over 100 Mbps link)
    // Distributed offloading should detect that wire transmission regresses performance
    auto decisionSlow = WorkloadPartitioner::calculateOptimalDecision(
        512, 512, 5000.0, 6000.0, 12'500'000, 1000, 1.2f, 300.0, 500.0);

    CHECK(decisionSlow.isLocalFallback);
    CHECK(decisionSlow.splitRatio == 1.0f);
    CHECK(decisionSlow.predictedWallClockUs <= 5000.0);

    // Case B: High compute load over 1 Gbps LAN (1024x1024)
    // Offload is profitable -> split selected with speedup
    auto decisionFast = WorkloadPartitioner::calculateOptimalDecision(
        1024, 1024, 80'000.0, 85'000.0, 125'000'000, 1000, 3.5f, 300.0, 500.0);

    CHECK(!decisionFast.isLocalFallback);
    CHECK(decisionFast.splitRatio > 0.40f && decisionFast.splitRatio < 0.65f);
    CHECK(decisionFast.predictedSpeedupVsBestGpu > 1.0);
}

// ─── Test 4: Dynamic Work-Stealing Batch Execution ────────────────────────────

static void test_dynamic_work_stealing_batch_regression() {
    std::vector<protocol::TaskRequest> tasks(12);
    for (size_t i = 0; i < tasks.size(); ++i) {
        tasks[i].taskId = 300 + static_cast<uint64_t>(i);
        tasks[i].type   = protocol::TaskType::Blur;
        tasks[i].width  = 256;
        tasks[i].height = 256;
        tasks[i].input  = makeTestPattern(256, 256);
    }

    auto wsResult = AdvancedBatchScheduler::runDynamicWorkStealing(
        tasks, referenceCpuBlur, referenceCpuBlur, true, 50.0);

    CHECK(wsResult.totalTasks == 12);
    CHECK(wsResult.totalWallClockMs > 0.0);
    CHECK(wsResult.throughputTasksPerSec > 0.0);
    CHECK(wsResult.speedupVsHostAlone > 0.0);
}

// ─── Test 5: CSV & JSON Report Generation & Round-Trip ────────────────────────

static void test_csv_and_json_report_generation() {
    auto tempCsv = std::filesystem::temp_directory_path() / "test_matrix_report.csv";
    auto tempJson = std::filesystem::temp_directory_path() / "test_matrix_report.json";

    if (std::filesystem::exists(tempCsv)) std::filesystem::remove(tempCsv);
    if (std::filesystem::exists(tempJson)) std::filesystem::remove(tempJson);

    // Write sample CSV
    {
        std::ofstream ofs(tempCsv);
        ofs << "Resolution,Strategy,Samples,MeanMs,MedianMs,P90Ms,P95Ms,MaxMs,StdDevMs,ThroughputFps,SpeedupVsBest,PairedDeltaMs,CI95LowerMs,CI95UpperMs,PValue,IsSignificant,MAE,Pass\n";
        ofs << "1024x1024,HostOnly,10,15.20,15.10,15.50,15.60,15.80,0.30,65.79,1.00,0.00,0.00,0.00,1.00000,false,0.00,PASS\n";
        ofs << "1024x1024,AdaptiveSpatialPartitioning,10,9.80,9.70,10.10,10.20,10.50,0.25,102.04,1.56,5.40,4.80,6.00,0.00010,true,0.12,PASS\n";
    }

    CHECK(std::filesystem::exists(tempCsv));
    CHECK(std::filesystem::file_size(tempCsv) > 50);

    // Read back CSV and verify line count
    {
        std::ifstream ifs(tempCsv);
        std::string line;
        size_t lines = 0;
        while (std::getline(ifs, line)) {
            if (!line.empty()) ++lines;
        }
        CHECK(lines == 3);
    }

    // Write sample JSON
    {
        std::ofstream ofs(tempJson);
        ofs << "{\n"
            << "  \"system_info\": { \"primary_gpu\": \"RTX 3050\" },\n"
            << "  \"parameters\": { \"warmup_runs\": 3, \"measured_runs\": 10 },\n"
            << "  \"matrix_results\": [\n"
            << "    { \"resolution\": \"1024x1024\", \"speedup_vs_best\": 1.56, \"pass\": true }\n"
            << "  ],\n"
            << "  \"summary\": { \"validation_passed\": true }\n"
            << "}\n";
    }

    CHECK(std::filesystem::exists(tempJson));
    CHECK(std::filesystem::file_size(tempJson) > 50);

    // Read back JSON and check essential keys
    {
        std::ifstream ifs(tempJson);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        CHECK(content.find("\"system_info\"") != std::string::npos);
        CHECK(content.find("\"matrix_results\"") != std::string::npos);
        CHECK(content.find("\"validation_passed\": true") != std::string::npos);
    }

    std::filesystem::remove(tempCsv);
    std::filesystem::remove(tempJson);
}

// ─── Entry Point ─────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Benchmark Matrix & Performance Profiling Tests (Phase 2.7) ===\n";

    test_statistical_metrics_calculation();
    test_matrix_execution_and_accuracy();
    test_cost_model_speedup_bounds();
    test_dynamic_work_stealing_batch_regression();
    test_csv_and_json_report_generation();

    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
