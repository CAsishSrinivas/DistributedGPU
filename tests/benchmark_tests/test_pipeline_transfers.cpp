// tests/benchmark_tests/test_pipeline_transfers.cpp
// Comprehensive unit and integration test suite for Phase 2.3:
// Pipeline Transfers, CPU LZ4 Codec Overlap, and Double-Buffered D3D12 Execution.

#include "apps/host/pipeline_executor.hpp"
#include "apps/host/batch_scheduler.hpp"
#include "apps/host/frame_compositor.hpp"
#include "d3d12/pipelined_gpu_executor.hpp"
#include "d3d12/gpu_executor.hpp"
#include "protocol/message.hpp"
#include "protocol/lz4.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace dgpu::host;
using namespace dgpu::gpu_executor;
using namespace dgpu::protocol;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << #expr << '\n'; \
            ++g_failed;                                                                \
        } else {                                                                       \
            ++g_passed;                                                                \
        }                                                                              \
    } while (0)

static std::vector<std::byte> makePattern(uint32_t w, uint32_t h) {
    std::vector<std::byte> data(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            data[idx + 0] = static_cast<std::byte>((x * 3 + y) & 0xFF);
            data[idx + 1] = static_cast<std::byte>((y * 5 + x) & 0xFF);
            data[idx + 2] = static_cast<std::byte>((x ^ y) & 0xFF);
            data[idx + 3] = static_cast<std::byte>(0xFF);
        }
    }
    return data;
}

// ─── Test 1: Bounded In-Flight Queue & Completion Integrity ──────────────────

static void test_pipeline_in_flight_bounding() {
    std::cout << "[Test 1] Testing Bounded In-Flight Queue & Ordering...\n";

    PipelineConfig cfg;
    cfg.inFlightDepth = 2; // double-buffering
    cfg.overlapCompression = true;
    cfg.overlapDecompression = true;

    PipelineExecutor executor(cfg);

    const size_t taskCount = 12;
    std::vector<TaskRequest> tasks;
    tasks.reserve(taskCount);

    for (size_t i = 0; i < taskCount; ++i) {
        TaskRequest req;
        req.taskId = i + 1;
        req.type = TaskType::Blur;
        req.width = 64;
        req.height = 64;
        req.input = makePattern(64, 64);
        tasks.push_back(std::move(req));
    }

    std::atomic<uint32_t> maxConcurrency{0};
    std::atomic<uint32_t> currentConcurrency{0};

    auto mockLocalExec = [&](const TaskRequest& r) -> TaskResult {
        uint32_t cur = ++currentConcurrency;
        uint32_t prevMax = maxConcurrency.load();
        while (cur > prevMax && !maxConcurrency.compare_exchange_weak(prevMax, cur)) {}

        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        TaskResult res;
        res.taskId = r.taskId;
        res.status = TaskStatus::Ok;
        res.output = r.input; // identity
        res.gpuUs = 5000;
        res.totalUs = 5200;

        --currentConcurrency;
        return res;
    };

    PipelineStageMetrics metrics;
    auto results = executor.executePipeline(tasks, mockLocalExec, nullptr, false, &metrics);

    CHECK(results.size() == taskCount);
    for (size_t i = 0; i < taskCount; ++i) {
        CHECK(results[i].taskId == tasks[i].taskId);
        CHECK(results[i].status == TaskStatus::Ok);
        CHECK(results[i].output == tasks[i].input);
    }

    // In-flight concurrency should be bounded
    CHECK(maxConcurrency.load() <= cfg.inFlightDepth + 1);
    CHECK(metrics.totalTasks == taskCount);
    CHECK(metrics.throughputTasksPerSec > 0.0);

    std::cout << "  Passed (Max concurrency observed: " << maxConcurrency.load()
              << ", Throughput: " << metrics.throughputTasksPerSec << " tasks/s)\n";
}

// ─── Test 2: Compression & Decompression Overlap Round-Trip ─────────────────

static void test_pipeline_compression_decompression_fidelity() {
    std::cout << "[Test 2] Testing LZ4 Compression/Decompression Overlap Fidelity...\n";

    PipelineConfig cfg;
    cfg.inFlightDepth = 3;
    cfg.overlapCompression = true;
    cfg.overlapDecompression = true;

    PipelineExecutor executor(cfg);

    const size_t taskCount = 8;
    std::vector<TaskRequest> tasks;
    for (size_t i = 0; i < taskCount; ++i) {
        TaskRequest req;
        req.taskId = 100 + i;
        req.type = TaskType::EdgeDetection;
        req.width = 128;
        req.height = 128;
        req.input = makePattern(128, 128);
        tasks.push_back(std::move(req));
    }

    auto mockLocalExec = [](const TaskRequest& r) -> TaskResult {
        TaskResult res;
        res.taskId = r.taskId;
        res.status = TaskStatus::Ok;
        res.output = r.input;
        res.gpuUs = 2000;
        res.totalUs = 2500;
        return res;
    };

    PipelineStageMetrics metrics;
    auto results = executor.executePipeline(tasks, mockLocalExec, nullptr, false, &metrics);

    CHECK(results.size() == taskCount);
    CHECK(metrics.totalCompressionUs > 0);
    CHECK(metrics.totalDecompressionUs > 0);

    for (size_t i = 0; i < taskCount; ++i) {
        CHECK(results[i].output == tasks[i].input);
    }

    std::cout << "  Passed (Compression time: " << metrics.totalCompressionUs
              << " us, Decompression time: " << metrics.totalDecompressionUs << " us)\n";
}

// ─── Test 3: Pipelined D3D12 Double-Buffering & Determinism ──────────────────

static void test_pipelined_d3d12_determinism() {
    std::cout << "[Test 3] Testing Pipelined D3D12 Double-Buffering Determinism...\n";

    PipelinedGpuExecutor pipelinedExec;
    GpuExecutor syncExec;

    if (!pipelinedExec.init("", 2)) {
        std::cout << "  [SKIP] D3D12 hardware not available for pipelined GPU test\n";
        return;
    }
    if (!syncExec.init("")) {
        std::cout << "  [SKIP] D3D12 hardware not available for sync GPU baseline\n";
        return;
    }

    const uint32_t w = 128;
    const uint32_t h = 128;
    const size_t taskCount = 6;

    std::vector<TaskRequest> tasks;
    tasks.reserve(taskCount);
    for (size_t i = 0; i < taskCount; ++i) {
        TaskRequest req;
        req.taskId = 200 + i;
        req.type = TaskType::EdgeDetection;
        req.width = w;
        req.height = h;
        req.input = makePattern(w, h);
        tasks.push_back(std::move(req));
    }

    // 1. Run synchronously via single-buffered GpuExecutor
    std::vector<TaskResult> syncResults;
    syncResults.reserve(taskCount);
    for (const auto& t : tasks) {
        syncResults.push_back(syncExec.execute(t));
    }

    // 2. Run via double-buffered PipelinedGpuExecutor
    PipelinedExecutionMetrics pipeMetrics;
    auto pipeResults = pipelinedExec.executePipelined(tasks, &pipeMetrics);

    CHECK(pipeResults.size() == taskCount);
    for (size_t i = 0; i < taskCount; ++i) {
        CHECK(pipeResults[i].status == TaskStatus::Ok);
        CHECK(syncResults[i].status == TaskStatus::Ok);
        CHECK(pipeResults[i].output.size() == syncResults[i].output.size());

        // Verify output bit-exactness between pipelined and sequential execution
        double mae = FrameCompositor::computeMAE(syncResults[i].output, pipeResults[i].output);
        CHECK(mae == 0.0);
    }

    // Also verify Blur task execution on pipelined executor
    TaskRequest blurReq;
    blurReq.taskId = 299;
    blurReq.type = TaskType::Blur;
    blurReq.width = w;
    blurReq.height = h;
    blurReq.input = makePattern(w, h);
    auto blurRes = pipelinedExec.execute(blurReq);
    CHECK(blurRes.status == TaskStatus::Ok);
    CHECK(blurRes.output.size() == static_cast<size_t>(w * h * 4));

    CHECK(pipeMetrics.slotCount == 2);
    CHECK(pipeMetrics.throughputTasksPerSec > 0.0);

    std::cout << "  Passed (Bit-exact match verified, MAE = 0.0 across all "
              << taskCount << " tasks, Throughput = " << pipeMetrics.throughputTasksPerSec << " t/s)\n";
}

// ─── Test 4: Batch Scheduler Integration & Percentile Metrics ────────────────

static void test_batch_scheduler_pipelined_run() {
    std::cout << "[Test 4] Testing AdvancedBatchScheduler Pipelined Batch Integration...\n";

    const uint32_t taskCount = 10;
    std::vector<TaskRequest> tasks;
    for (uint32_t i = 0; i < taskCount; ++i) {
        TaskRequest req;
        req.taskId = 300 + i;
        req.type = TaskType::Blur;
        req.width = 64;
        req.height = 64;
        req.input = makePattern(64, 64);
        tasks.push_back(std::move(req));
    }

    auto mockExec = [](const TaskRequest& r) -> TaskResult {
        TaskResult res;
        res.taskId = r.taskId;
        res.status = TaskStatus::Ok;
        res.output = r.input;
        res.gpuUs = 3000;
        res.totalUs = 3500;
        return res;
    };

    auto metrics = AdvancedBatchScheduler::runPipelinedBatch(
        tasks, mockExec, mockExec, false, 2, 200.0);

    CHECK(metrics.totalTasks == taskCount);
    CHECK(metrics.hostTasksExecuted == taskCount);
    CHECK(metrics.failedTasks == 0);
    CHECK(metrics.totalWallClockMs > 0.0);
    CHECK(metrics.throughputTasksPerSec > 0.0);
    CHECK(metrics.p50LatencyMs > 0.0);
    CHECK(metrics.p95LatencyMs >= metrics.p50LatencyMs);
    CHECK(metrics.maxLatencyMs >= metrics.p95LatencyMs);
    CHECK(metrics.inFlightDepth == 2);

    std::cout << "  Passed (Wall clock: " << metrics.totalWallClockMs
              << " ms, Throughput: " << metrics.throughputTasksPerSec
              << " t/s, P50: " << metrics.p50LatencyMs
              << " ms, P95: " << metrics.p95LatencyMs << " ms)\n";
}

// ─── Test 5: Dynamic Resolution Stress & Resource Lifecycle ──────────────────

static void test_resolution_scaling_and_lifecycle() {
    std::cout << "[Test 5] Testing Dynamic Resolution Scaling & Buffer Reallocation...\n";

    PipelinedGpuExecutor exec;
    if (!exec.init("", 3)) { // 3-stage multi-buffering
        std::cout << "  [SKIP] D3D12 hardware not available for multi-buffering lifecycle test\n";
        return;
    }

    // Sequence of varying resolutions
    std::vector<std::pair<uint32_t, uint32_t>> resolutions = {
        {64, 64}, {128, 128}, {64, 64}, {256, 256}, {128, 128}, {64, 64}
    };

    std::vector<TaskRequest> tasks;
    for (size_t i = 0; i < resolutions.size(); ++i) {
        TaskRequest req;
        req.taskId = 400 + i;
        req.type = (i % 2 == 0) ? TaskType::Blur : TaskType::EdgeDetection;
        req.width = resolutions[i].first;
        req.height = resolutions[i].second;
        req.input = makePattern(req.width, req.height);
        tasks.push_back(std::move(req));
    }

    PipelinedExecutionMetrics metrics;
    auto results = exec.executePipelined(tasks, &metrics);

    CHECK(results.size() == resolutions.size());
    for (size_t i = 0; i < results.size(); ++i) {
        CHECK(results[i].status == TaskStatus::Ok);
        size_t expectedBytes = static_cast<size_t>(resolutions[i].first) * resolutions[i].second * 4;
        CHECK(results[i].output.size() == expectedBytes);
    }

    std::cout << "  Passed (Successfully reallocated buffers across "
              << resolutions.size() << " dynamic resolution transitions)\n";
}

int main() {
    try {
        std::cout << "====================================================\n";
        std::cout << "  DistributedGPU Phase 2.3: Pipeline Execution Tests\n";
        std::cout << "====================================================\n" << std::flush;

        test_pipeline_in_flight_bounding();
        std::cout << std::flush;
        test_pipeline_compression_decompression_fidelity();
        std::cout << std::flush;
        test_pipelined_d3d12_determinism();
        std::cout << std::flush;
        test_batch_scheduler_pipelined_run();
        std::cout << std::flush;
        test_resolution_scaling_and_lifecycle();
        std::cout << std::flush;

        std::cout << "====================================================\n";
        std::cout << "  Results: " << g_passed << " passed, " << g_failed << " failed\n";
        std::cout << "====================================================\n" << std::flush;
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION in main: " << e.what() << '\n' << std::flush;
        return 1;
    } catch (...) {
        std::cerr << "UNKNOWN EXCEPTION in main\n" << std::flush;
        return 1;
    }

    return (g_failed == 0) ? 0 : 1;
}
