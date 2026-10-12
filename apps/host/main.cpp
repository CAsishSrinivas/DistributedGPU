// apps/host/main.cpp
// Host process: executes primary 3D scene rendering on RTX 3050,
// offloads compute/post-process passes locally or remotely (GTX 1650 Ti),
// composites frames via FrameCompositor, schedules adaptively via JobScheduler,
// and produces the complete M4/M5 distributed rendering benchmark report.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "networking/tcp_connection.hpp"
#include "networking/health_monitor.hpp"
#include "protocol/message.hpp"
#include "d3d12/d3d12_context.hpp"
#include "d3d12/gpu_executor.hpp"
#include "task_runtime/task_runtime.hpp"
#include "telemetry/telemetry.hpp"

#include "host_server.hpp"
#include "job_scheduler.hpp"
#include "frame_compositor.hpp"
#include "workload_partitioner.hpp"
#include "statistical_validator.hpp"
#include "batch_scheduler.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

// ─── Configuration ────────────────────────────────────────────────────────────

static constexpr const char* DEFAULT_WORKER_HOST = "127.0.0.1";
static constexpr uint16_t    DEFAULT_WORKER_PORT = 7777;
static constexpr const char* PREFERRED_ADAPTER   = "RTX 3050";

// Benchmark matrix: (width, height, runs, warmup)
struct BenchConfig {
    uint32_t width;
    uint32_t height;
    uint32_t runs;
    uint32_t warmup;
};

static constexpr std::array<BenchConfig, 4> kBenchConfigs{{
    {  256,  256, 30, 5},
    {  512,  512, 30, 5},
    { 1024, 1024, 20, 3},
    { 1920, 1080, 15, 2},
}};

// ─── Host Primary 3D Scene Renderer (M4) ──────────────────────────────────────
// Simulates the host's primary 3D rendering workload (e.g., raymarched geometry,
// lighting, specular highlights, texturing) to produce an intermediate HDR/RGBA8 buffer.
// sceneType: 0 = smooth procedural gradient (high compressibility)
//            1 = high-frequency pseudo-random hash noise (low compressibility)

static std::vector<std::byte> renderHostScene(uint32_t width, uint32_t height, uint32_t frameIdx, int sceneType = 0) {
    std::vector<std::byte> frame(static_cast<size_t>(width) * height * 4);
    const uint8_t pulse = static_cast<uint8_t>(128.0 + 60.0 * std::sin(frameIdx * 0.1f));

    if (sceneType == 1) { // High-frequency pseudo-random noise (low compressibility, ~1.0x ratio)
        for (uint32_t y = 0; y < height; ++y) {
            size_t rowOffset = static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t h = (x * 374761393u) ^ (y * 668265263u) ^ (frameIdx * 3628273133u);
                h = (h ^ (h >> 13)) * 1274126177u;
                size_t idx = rowOffset + x * 4;
                frame[idx + 0] = static_cast<std::byte>(h & 0xFF);
                frame[idx + 1] = static_cast<std::byte>((h >> 8) & 0xFF);
                frame[idx + 2] = static_cast<std::byte>((h >> 16) & 0xFF);
                frame[idx + 3] = static_cast<std::byte>(255);
            }
        }
    } else { // Realistic rendered game frame with skybox, silhouettes, and UI overlay (high compressibility, ~3x-5x ratio)
        for (uint32_t y = 0; y < height; ++y) {
            size_t rowOffset = static_cast<size_t>(y) * width * 4;
            if (y < height * 45 / 100) {
                // Sky bands (identical 4-byte pixels across each band)
                uint8_t band = static_cast<uint8_t>((y * 16) / (height * 45 / 100 + 1));
                uint8_t r = static_cast<uint8_t>(40 + band * 3);
                uint8_t g = static_cast<uint8_t>(85 + band * 5);
                uint8_t b = static_cast<uint8_t>(180 + band * 3);
                for (uint32_t x = 0; x < width; ++x) {
                    size_t idx = rowOffset + x * 4;
                    frame[idx + 0] = static_cast<std::byte>(r);
                    frame[idx + 1] = static_cast<std::byte>(g);
                    frame[idx + 2] = static_cast<std::byte>(b);
                    frame[idx + 3] = static_cast<std::byte>(255);
                }
            } else if (y > height * 88 / 100) {
                // UI & HUD overlay (dark letterbox bar with health meter)
                for (uint32_t x = 0; x < width; ++x) {
                    size_t idx = rowOffset + x * 4;
                    if (x > width * 10 / 100 && x < width * 45 / 100 && y > height * 91 / 100 && y < height * 95 / 100) {
                        frame[idx + 0] = static_cast<std::byte>(35);
                        frame[idx + 1] = static_cast<std::byte>(215);
                        frame[idx + 2] = static_cast<std::byte>(45);
                    } else {
                        frame[idx + 0] = static_cast<std::byte>(18);
                        frame[idx + 1] = static_cast<std::byte>(18);
                        frame[idx + 2] = static_cast<std::byte>(22);
                    }
                    frame[idx + 3] = static_cast<std::byte>(255);
                }
            } else {
                // Geometry terrain blocks with quantized lighting
                uint8_t groundVal = static_cast<uint8_t>(60 + ((y * 12) / height) * 10);
                for (uint32_t x = 0; x < width; ++x) {
                    size_t idx = rowOffset + x * 4;
                    uint8_t tile = static_cast<uint8_t>((x / 32) % 2 == 0 ? 0 : 20);
                    frame[idx + 0] = static_cast<std::byte>(groundVal + tile);
                    frame[idx + 1] = static_cast<std::byte>(groundVal + tile + 8);
                    frame[idx + 2] = static_cast<std::byte>(groundVal);
                    frame[idx + 3] = static_cast<std::byte>(255);
                }
            }
        }
    }
    return frame;
}

// ─── Multi-Repetition Statistical Framework (Priority 4) ──────────────────────

struct BenchmarkStats {
    std::vector<double> samplesMs;
    double minMs{0.0};
    double maxMs{0.0};
    double meanMs{0.0};
    double medianMs{0.0};
    double p95Ms{0.0};
    double stdDevMs{0.0};
    double cvPercent{0.0}; // Coefficient of Variation = (StdDev / Mean) * 100%

    static BenchmarkStats compute(std::vector<double> samples) {
        if (samples.empty()) return {};
        std::sort(samples.begin(), samples.end());
        BenchmarkStats s;
        s.samplesMs = samples;
        s.minMs = samples.front();
        s.maxMs = samples.back();
        double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        s.meanMs = sum / samples.size();

        size_t n = samples.size();
        if (n % 2 == 1) {
            s.medianMs = samples[n / 2];
        } else {
            s.medianMs = (samples[n / 2 - 1] + samples[n / 2]) * 0.5;
        }

        size_t p95Idx = static_cast<size_t>(std::ceil(0.95 * n)) - 1;
        if (p95Idx >= n) p95Idx = n - 1;
        s.p95Ms = samples[p95Idx];

        double sqDiffSum = 0.0;
        for (double v : samples) {
            sqDiffSum += (v - s.meanMs) * (v - s.meanMs);
        }
        s.stdDevMs = std::sqrt(sqDiffSum / n);
        s.cvPercent = (s.meanMs > 0.0) ? (s.stdDevMs / s.meanMs * 100.0) : 0.0;
        return s;
    }
};

// ─── Benchmark Runner ─────────────────────────────────────────────────────────

static void runDistributedBenchmark(const BenchConfig& cfg,
                                    dgpu::gpu_executor::GpuExecutor& localExec,
                                    dgpu::networking::TaskClient&    remoteClient,
                                    dgpu::host::JobScheduler&        scheduler,
                                    dgpu::host::FrameCompositor&     compositor,
                                    bool                             remoteAvailable) {
    using namespace dgpu;
    using namespace dgpu::protocol;
    namespace tel = dgpu::telemetry;

    std::cout << std::format("\n╔══════════════════════════════════════════════════════════════════════╗\n");
    std::cout << std::format("║ Resolution: {:>4}x{:<4}  (Total passes: {})                          ║\n",
                             cfg.width, cfg.height, cfg.runs);
    std::cout << std::format("╚══════════════════════════════════════════════════════════════════════╝\n");

    std::vector<std::byte> lastLocalComposited;
    std::vector<std::byte> lastRemoteComposited;

    // ──────────────────────────────────────────────────────────────────────────
    // Configuration 1: Local GPU Execution (Host RTX 3050 does all passes)
    // ──────────────────────────────────────────────────────────────────────────
    {
        uint64_t taskId = 1000;
        for (uint32_t i = 0; i < cfg.warmup + cfg.runs; ++i) {
            // 1. Host renders primary 3D scene
            auto hostStart = std::chrono::steady_clock::now();
            auto baseFrame = renderHostScene(cfg.width, cfg.height, i);
            auto hostEnd   = std::chrono::steady_clock::now();
            uint64_t hostRenderUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        hostEnd - hostStart).count();

            // 2. Local GPU executes filter pass
            TaskRequest req;
            req.taskId = taskId++;
            req.type   = TaskType::Blur;
            req.width  = cfg.width;
            req.height = cfg.height;
            req.input  = baseFrame;

            auto execStart = std::chrono::steady_clock::now();
            auto result    = localExec.execute(req);
            auto execEnd   = std::chrono::steady_clock::now();
            uint64_t localGpuTotalUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                           execEnd - execStart).count();

            // 3. Composite post-processed result over base frame
            auto compStart = std::chrono::steady_clock::now();
            compositor.composite(baseFrame, result.output, cfg.width, cfg.height,
                                 host::CompositeMode::AlphaBlend, 0.75f);
            auto compEnd   = std::chrono::steady_clock::now();
            uint64_t compUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  compEnd - compStart).count();

            if (i >= cfg.warmup) {
                tel::FrameRecord fr;
                fr.frameIndex  = i - cfg.warmup;
                fr.label       = std::format("local_{}x{}", cfg.width, cfg.height);
                fr.isRemote    = false;
                fr.width       = cfg.width;
                fr.height      = cfg.height;
                fr.uploadUs    = result.uploadUs;
                fr.gpuExecUs   = result.gpuUs;
                fr.readbackUs  = result.readbackUs;
                fr.totalUs     = hostRenderUs + localGpuTotalUs + compUs;
                tel::Telemetry::instance().record(fr);

                scheduler.recordLocalSuccess(cfg.width, cfg.height, localGpuTotalUs, baseFrame.size());
            }

            if (i == cfg.warmup + cfg.runs - 1) {
                lastLocalComposited = std::move(baseFrame);
            }
        }

        auto rep = tel::Telemetry::instance().summarise(
            std::format("local_{}x{}", cfg.width, cfg.height));
        std::cout << std::format("  [1] Local GPU (RTX 3050):  mean={:>8.0f}us  p95={:>8.0f}us  fps={:>6.1f}\n",
                                 rep.meanUs, rep.p95Us, rep.avgFps);
    }

    if (!remoteAvailable) {
        std::cout << "  [2] Remote Sync:           SKIPPED (worker not connected)\n";
        std::cout << "  [3] Remote Async:          SKIPPED (worker not connected)\n";
        std::cout << "  [4] Adaptive Scheduler:    SKIPPED (worker not connected)\n";
        return;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Configuration 2: Remote Synchronous Execution (Worker GTX 1650 Ti)
    // ──────────────────────────────────────────────────────────────────────────
    {
        uint64_t taskId = 2000;
        for (uint32_t i = 0; i < cfg.warmup + cfg.runs; ++i) {
            auto hostStart = std::chrono::steady_clock::now();
            auto baseFrame = renderHostScene(cfg.width, cfg.height, i);
            auto hostEnd   = std::chrono::steady_clock::now();
            uint64_t hostRenderUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        hostEnd - hostStart).count();

            TaskRequest req;
            req.taskId = taskId++;
            req.type   = TaskType::Blur;
            req.width  = cfg.width;
            req.height = cfg.height;
            req.input  = baseFrame;

            auto netStart = std::chrono::steady_clock::now();
            auto result   = remoteClient.submit(req);
            auto netEnd   = std::chrono::steady_clock::now();

            uint64_t roundTripUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                       netEnd - netStart).count();
            uint64_t netUs = (roundTripUs > result.totalUs) ? (roundTripUs - result.totalUs) : 0;

            auto compStart = std::chrono::steady_clock::now();
            compositor.composite(baseFrame, result.output, cfg.width, cfg.height,
                                 host::CompositeMode::AlphaBlend, 0.75f);
            auto compEnd   = std::chrono::steady_clock::now();
            uint64_t compUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  compEnd - compStart).count();

            if (i >= cfg.warmup) {
                tel::FrameRecord fr;
                fr.frameIndex      = i - cfg.warmup;
                fr.label           = std::format("remote_sync_{}x{}", cfg.width, cfg.height);
                fr.isRemote        = true;
                fr.width           = cfg.width;
                fr.height          = cfg.height;
                fr.uploadUs        = result.uploadUs;
                fr.gpuExecUs       = result.gpuUs;
                fr.readbackUs      = result.readbackUs;
                fr.networkSendUs   = netUs / 2;
                fr.networkReturnUs = netUs / 2;
                fr.totalUs         = hostRenderUs + roundTripUs + compUs;
                tel::Telemetry::instance().record(fr);

                scheduler.recordRemoteSuccess(cfg.width, cfg.height, result.gpuUs, netUs);
            }

            if (i == cfg.warmup + cfg.runs - 1) {
                lastRemoteComposited = std::move(baseFrame);
            }
        }

        auto rep = tel::Telemetry::instance().summarise(
            std::format("remote_sync_{}x{}", cfg.width, cfg.height));

        double mae = 0.0;
        if (!lastLocalComposited.empty() && !lastRemoteComposited.empty()) {
            mae = host::FrameCompositor::computeMAE(lastLocalComposited, lastRemoteComposited);
        }

        std::cout << std::format("  [2] Remote Sync (1650 Ti): mean={:>8.0f}us  p95={:>8.0f}us  fps={:>6.1f}  (MAE={:.2f})\n",
                                 rep.meanUs, rep.p95Us, rep.avgFps, mae);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Configuration 3: Remote Asynchronous Execution (True 1-Frame Double-Buffered Pipeline)
    // ──────────────────────────────────────────────────────────────────────────
    {
        uint64_t taskId = 3000;
        const uint32_t totalFrames = cfg.warmup + cfg.runs;

        // Seed pipeline: render frame 0 and submit to remote worker
        auto prevBaseFrame = renderHostScene(cfg.width, cfg.height, 0);
        TaskRequest req0;
        req0.taskId = taskId++;
        req0.type   = TaskType::Blur;
        req0.width  = cfg.width;
        req0.height = cfg.height;
        req0.input  = prevBaseFrame;

        auto inFlightJob = scheduler.submit(std::move(req0), task_runtime::ExecutionSite::Remote);

        // Pipelined frame loop: host renders frame i CONCURRENTLY while worker processes frame i-1
        for (uint32_t i = 1; i <= totalFrames; ++i) {
            auto frameStart = std::chrono::steady_clock::now();

            // 1. Host renders frame i concurrently with worker processing frame i-1
            std::vector<std::byte> currBaseFrame;
            if (i < totalFrames) {
                currBaseFrame = renderHostScene(cfg.width, cfg.height, i);
            }

            // 2. Await remote result for frame i-1
            auto prevResult = inFlightJob.future.get();

            // 3. Composite frame i-1
            compositor.composite(prevBaseFrame, prevResult.output, cfg.width, cfg.height,
                                 host::CompositeMode::AlphaBlend, 0.75f);

            auto frameEnd = std::chrono::steady_clock::now();
            uint64_t frameTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                       frameEnd - frameStart).count();

            if (i > cfg.warmup) {
                tel::FrameRecord fr;
                fr.frameIndex  = i - 1 - cfg.warmup;
                fr.label       = std::format("remote_async_{}x{}", cfg.width, cfg.height);
                fr.isRemote    = true;
                fr.width       = cfg.width;
                fr.height      = cfg.height;
                fr.gpuExecUs   = prevResult.gpuUs;
                fr.totalUs     = frameTimeUs;
                tel::Telemetry::instance().record(fr);
            }

            // 4. Submit frame i to worker for next overlap cycle
            if (i < totalFrames) {
                TaskRequest reqNext;
                reqNext.taskId = taskId++;
                reqNext.type   = TaskType::Blur;
                reqNext.width  = cfg.width;
                reqNext.height = cfg.height;
                reqNext.input  = currBaseFrame;

                inFlightJob   = scheduler.submit(std::move(reqNext), task_runtime::ExecutionSite::Remote);
                prevBaseFrame = std::move(currBaseFrame);
            }
        }

        auto rep = tel::Telemetry::instance().summarise(
            std::format("remote_async_{}x{}", cfg.width, cfg.height));
        std::cout << std::format("  [3] Remote Async (Pipelined):  mean={:>8.0f}us  p95={:>8.0f}us  fps={:>6.1f}\n",
                                 rep.meanUs, rep.p95Us, rep.avgFps);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Configuration 4: JobScheduler Adaptive Decision (M5)
    // ──────────────────────────────────────────────────────────────────────────
    {
        uint64_t taskId = 4000;
        for (uint32_t i = 0; i < cfg.warmup + cfg.runs; ++i) {
            auto frameStart = std::chrono::steady_clock::now();
            auto baseFrame  = renderHostScene(cfg.width, cfg.height, i);

            TaskRequest req;
            req.taskId = taskId++;
            req.type   = TaskType::Blur;
            req.width  = cfg.width;
            req.height = cfg.height;
            req.input  = baseFrame;

            // Submit with NO forced site — JobScheduler evaluates EMA & circuit breaker!
            auto job    = scheduler.submit(std::move(req));
            auto result = job.future.get();

            compositor.composite(baseFrame, result.output, cfg.width, cfg.height,
                                 host::CompositeMode::AlphaBlend, 0.75f);

            auto frameEnd = std::chrono::steady_clock::now();
            uint64_t totalUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                   frameEnd - frameStart).count();

            if (i >= cfg.warmup) {
                tel::FrameRecord fr;
                fr.frameIndex = i - cfg.warmup;
                fr.label      = std::format("adaptive_{}x{}", cfg.width, cfg.height);
                fr.isRemote   = (job.site == task_runtime::ExecutionSite::Remote);
                fr.width      = cfg.width;
                fr.height     = cfg.height;
                fr.gpuExecUs  = result.gpuUs;
                fr.totalUs    = totalUs;
                tel::Telemetry::instance().record(fr);
            }
        }

        auto rep = tel::Telemetry::instance().summarise(
            std::format("adaptive_{}x{}", cfg.width, cfg.height));
        std::cout << std::format("  [4] Adaptive Scheduler:     mean={:>8.0f}us  p95={:>8.0f}us  fps={:>6.1f}\n",
                                 rep.meanUs, rep.p95Us, rep.avgFps);
    }
}

// ─── JSON Benchmark Export Helper ───────────────────────────────────────────

struct JsonExportData {
    std::string testName;
    std::string hardwareHost;
    std::string hardwareWorker;
    std::string osName;
    uint32_t linkMbps{100};
    uint32_t linkLatencyMs{1};
    std::vector<std::string> jsonSections;

    void addSection(const std::string& sec) { jsonSections.push_back(sec); }

    void writeToFile(const std::filesystem::path& path) const {
        std::ofstream ofs(path);
        if (!ofs.is_open()) return;
        ofs << "{\n";
        ofs << "  \"benchmark\": \"" << testName << "\",\n";
        ofs << "  \"timestamp\": \"" << std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) << "\",\n";
        ofs << "  \"environment\": {\n";
        ofs << "    \"host_gpu\": \"" << hardwareHost << "\",\n";
        ofs << "    \"worker_gpu\": \"" << hardwareWorker << "\",\n";
        ofs << "    \"link_bandwidth_mbps\": " << linkMbps << ",\n";
        ofs << "    \"base_rtt_ms\": " << linkLatencyMs << "\n";
        ofs << "  },\n";
        ofs << "  \"results\": [\n";
        for (size_t i = 0; i < jsonSections.size(); ++i) {
            ofs << jsonSections[i];
            if (i + 1 < jsonSections.size()) ofs << ",";
            ofs << "\n";
        }
        ofs << "  ]\n";
        ofs << "}\n";
    }
};

static JsonExportData g_jsonExport;

// ─── Stage 5: Independent Parallel Workloads Benchmark ────────────────────────

static void runStage5ParallelWorkload(dgpu::gpu_executor::GpuExecutor& localExec,
                                      dgpu::networking::TaskClient&    remoteClient,
                                      bool                             remoteAvailable,
                                      uint64_t                         linkBandwidthBps = 12'500'000,
                                      uint64_t                         baseRttUs = 1000,
                                      uint32_t                         repetitions = 15,
                                      uint32_t                         warmup = 3,
                                      int                              sceneType = 0) {
    using namespace dgpu;
    using namespace dgpu::protocol;
    namespace tel = dgpu::telemetry;

    std::cout << "\n╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║ Stage 5: Cooperative Multi-GPU Speedup & Throughput Benchmark        ║\n";
    std::cout << "║ Evaluating 4 Modes against Fastest Single GPU Baseline               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";
    std::cout << std::format("  Configuration: {} runs (after {} warm-up runs)\n", repetitions, warmup);
    std::cout << std::format("  Scene Texture: {}\n", (sceneType == 1 ? "Procedural High-Frequency Noise (Low Compressibility)" : "Procedural Smooth Gradient (High Compressibility)"));
    std::cout << std::format("  Payload Codec: {}\n", (remoteClient.isCompressionEnabled() ? "Software LZ4 CPU Block Codec (Enabled)" : "Uncompressed Raw RGBA8 Wire"));

    auto localExecFn = [&localExec](const TaskRequest& req) { return localExec.execute(req); };
    auto remoteExecFn = [&remoteClient](const TaskRequest& req) { return remoteClient.submit(req); };

    // ──────────────────────────────────────────────────────────────────────────
    // Part A: Advanced Batch Scheduling & Throughput Optimization (Priority 2)
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "\n========================================================================\n";
    std::cout << " PART A: ADVANCED BATCH SCHEDULING & THROUGHPUT OPTIMIZATION\n";
    std::cout << "========================================================================\n";

    const uint32_t batchSize = 24;

    // 1. Allocation Sweep (0/24 to 24/0 in steps of 4) to map efficiency curve
    std::cout << "\n[A.1] Mapping Static Allocation Efficiency Curve (24 tasks of 256x256 Blur):\n";
    std::cout << "      Sweeping allocation split from 0/24 through 24/0 to determine optimal balance...\n";

    std::vector<TaskRequest> sweepBatch(batchSize);
    for (uint32_t i = 0; i < batchSize; ++i) {
        sweepBatch[i].taskId = 5000 + i;
        sweepBatch[i].type   = TaskType::Blur;
        sweepBatch[i].width  = 256;
        sweepBatch[i].height = 256;
        sweepBatch[i].input  = renderHostScene(256, 256, i * 7 + 1, sceneType);
    }

    auto sweepResults = host::AdvancedBatchScheduler::runAllocationSweep(
        sweepBatch, localExecFn, remoteExecFn, remoteAvailable, 4);
    host::AdvancedBatchScheduler::printSweepTable(sweepResults);

    // Find the best sweep point
    auto bestSweepIt = std::max_element(sweepResults.begin(), sweepResults.end(),
        [](const auto& a, const auto& b) { return a.throughputTasksPerSec < b.throughputTasksPerSec; });

    if (bestSweepIt != sweepResults.end()) {
        std::cout << std::format("  -> Optimal Static Allocation: {:>2} Host / {:>2} Worker ({:.2f} ms total, {:.1f} tasks/sec, {:.2f}x vs Host Alone)\n",
                                 bestSweepIt->hostTaskCount, bestSweepIt->workerTaskCount,
                                 bestSweepIt->totalWallClockMs, bestSweepIt->throughputTasksPerSec,
                                 bestSweepIt->speedupVsHostAlone);
    }

    // 2. Dynamic Work-Stealing with Bounded Queue & Tail Latency Distribution
    std::cout << "\n[A.2] Evaluating Dynamic Work-Stealing vs Baselines (24 tasks of 256x256 Blur):\n";
    std::cout << "      Measuring tail latency (P50, P90, P95, P99, Max) and queue wait times...\n";

    // Establish reference ground truth results for MAE check
    std::vector<TaskResult> refResults(batchSize);
    for (uint32_t i = 0; i < batchSize; ++i) {
        refResults[i] = localExec.execute(sweepBatch[i]);
    }

    auto hostMetrics = host::AdvancedBatchScheduler::runHostOnly(sweepBatch, localExecFn);
    double hostThroughput = hostMetrics.throughputTasksPerSec;

    auto static5050Metrics = host::AdvancedBatchScheduler::runStaticSplit(
        sweepBatch, 12, localExecFn, remoteExecFn, remoteAvailable, hostThroughput, refResults);

    auto optimalStaticMetrics = host::AdvancedBatchScheduler::runStaticSplit(
        sweepBatch, (bestSweepIt != sweepResults.end()) ? bestSweepIt->hostTaskCount : 12,
        localExecFn, remoteExecFn, remoteAvailable, hostThroughput, refResults);

    auto dynamicMetrics = host::AdvancedBatchScheduler::runDynamicWorkStealing(
        sweepBatch, localExecFn, remoteExecFn, remoteAvailable, hostThroughput, refResults);

    auto adaptiveMetrics = host::AdvancedBatchScheduler::runAdaptiveWorkStealing(
        sweepBatch, localExecFn, remoteExecFn, remoteAvailable,
        remoteClient.hasCapabilities() ? std::make_optional(remoteClient.capabilities()) : std::nullopt,
        12'500'000, 1000, hostThroughput, refResults);

    auto pipelinedMetrics = host::AdvancedBatchScheduler::runPipelinedBatch(
        sweepBatch, localExecFn, remoteExecFn, remoteAvailable, 2, hostThroughput, refResults);

    std::cout << "\n  ┌──────────────────────────────┬────────────┬────────────┬─────────┬────────────┬────────────┬────────────┐\n";
    std::cout << "  │ Batch Scheduling Strategy    │ Batch Time │ Throughput │ Speedup │ P50 Latency│ P95 Latency│ Max (Tail) │\n";
    std::cout << "  ├──────────────────────────────┼────────────┼────────────┼─────────┼────────────┼────────────┼────────────┤\n";
    host::AdvancedBatchScheduler::printMetricsSummary(hostMetrics);
    host::AdvancedBatchScheduler::printMetricsSummary(static5050Metrics);
    host::AdvancedBatchScheduler::printMetricsSummary(optimalStaticMetrics);
    host::AdvancedBatchScheduler::printMetricsSummary(dynamicMetrics);
    host::AdvancedBatchScheduler::printMetricsSummary(adaptiveMetrics);
    host::AdvancedBatchScheduler::printMetricsSummary(pipelinedMetrics);
    std::cout << "  └──────────────────────────────┴────────────┴────────────┴─────────┴────────────┴────────────┴────────────┘\n";
    std::cout << std::format("  -> Dynamic Work-Stealing Split: Host (RTX 3050) = {} tasks, Worker (GTX 1650 Ti) = {} tasks\n",
                             dynamicMetrics.hostTasksExecuted, dynamicMetrics.workerTasksExecuted);
    std::cout << std::format("  -> Mean Queue Wait Time:        {:.2f} ms | Visual Output Seam MAE: {:.2f}\n",
                             dynamicMetrics.meanQueueWaitMs, dynamicMetrics.maeVsReference);

    // 3. Multi-Workload Matrix (Different Task Sizes & Kernel Types)
    std::cout << "\n[A.3] Multi-Workload Matrix (Evaluating Task Sizes & Compute Kernels):\n";
    std::cout << "      Identifies the crossover where remote offloading transitions from overhead to win...\n";

    struct WorkloadMatrixItem {
        std::string name;
        uint32_t width;
        uint32_t height;
        uint32_t count;
        TaskType type;
    };

    const std::array<WorkloadMatrixItem, 4> workloadMatrix = {{
        {"128x128 Gaussian Blur (Small)",        128, 128, 24, TaskType::Blur},
        {"256x256 Gaussian Blur (Medium)",       256, 256, 24, TaskType::Blur},
        {"512x512 Gaussian Blur (Heavy)",        512, 512, 12, TaskType::Blur},
        {"256x256 Sobel Edge Detect (Kernel 2)", 256, 256, 24, TaskType::EdgeDetection}
    }};

    std::cout << "\n  ┌──────────────────────────────────────┬─────────────┬─────────────┬─────────────┬─────────┬──────────────┐\n";
    std::cout << "  │ Workload Matrix Item                 │ Host Alone  │ Static Split│ Dynamic Steal│ Speedup │ Assessment   │\n";
    std::cout << "  ├──────────────────────────────────────┼─────────────┼─────────────┼─────────────┼─────────┼──────────────┤\n";

    for (const auto& wItem : workloadMatrix) {
        std::vector<TaskRequest> wTasks(wItem.count);
        for (uint32_t i = 0; i < wItem.count; ++i) {
            wTasks[i].taskId = 7000 + i;
            wTasks[i].type   = wItem.type;
            wTasks[i].width  = wItem.width;
            wTasks[i].height = wItem.height;
            wTasks[i].input  = renderHostScene(wItem.width, wItem.height, i * 5 + 1, sceneType);
        }

        auto wHost = host::AdvancedBatchScheduler::runHostOnly(wTasks, localExecFn);
        auto wStatic = host::AdvancedBatchScheduler::runStaticSplit(
            wTasks, wItem.count / 2, localExecFn, remoteExecFn, remoteAvailable, wHost.throughputTasksPerSec);
        auto wDynamic = host::AdvancedBatchScheduler::runDynamicWorkStealing(
            wTasks, localExecFn, remoteExecFn, remoteAvailable, wHost.throughputTasksPerSec);

        std::string assessment = (wDynamic.speedupVsHostAlone >= 1.05) ? "GENUINE WIN" :
                                 (wDynamic.speedupVsHostAlone >= 0.98) ? "BREAK-EVEN " : "TRANSFER BOUND";

        std::cout << std::format("  │ {:<36} │ {:>8.1f} t/s│ {:>8.1f} t/s│ {:>8.1f} t/s│ {:>6.2f}x │ {:<12} │\n",
                                 wItem.name, wHost.throughputTasksPerSec, wStatic.throughputTasksPerSec,
                                 wDynamic.throughputTasksPerSec, wDynamic.speedupVsHostAlone, assessment);
    }
    std::cout << "  └──────────────────────────────────────┴─────────────┴─────────────┴─────────────┴─────────┴──────────────┘\n";

    // ──────────────────────────────────────────────────────────────────────────
    // Part B: Rigorous Statistical Validation (Interleaved Round-Robin Order)
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "\n========================================================================\n";
    std::cout << " PART B: RIGOROUS STATISTICAL VALIDATION (INTERLEAVED ROUND-ROBIN)\n";
    std::cout << "========================================================================\n";
    std::cout << "Testing 512x512, 1024x1024, and 1920x1080 across 5 execution modes with\n";
    std::cout << "alternating/interleaved execution order to eliminate GPU boost-clock bias.\n";

    const std::array<std::pair<uint32_t, uint32_t>, 3> slabResolutions = {{
        {512, 512},
        {1024, 1024},
        {1920, 1080}
    }};

    for (const auto& [fullW, fullH] : slabResolutions) {
        std::cout << std::format("\n╔══════════════════════════════════════════════════════════════════════╗\n");
        std::cout << std::format("║ Workload: {:>4}x{:<4} RGBA8 ({:.2f} MB uncompressed)                      ║\n",
                                 fullW, fullH, (static_cast<double>(fullW) * fullH * 4) / (1024.0 * 1024.0));
        std::cout << std::format("╚══════════════════════════════════════════════════════════════════════╝\n");
        std::cout << std::format("  Interleaved Repetitions: {} runs (after {} warm-up runs), Scene: {}\n",
                                 repetitions, warmup, (sceneType == 1 ? "Procedural High-Frequency Noise" : "Procedural Realistic Game Frame"));

        auto fullImg = renderHostScene(fullW, fullH, 42, sceneType);

        TaskRequest fullReq;
        fullReq.taskId = 8000;
        fullReq.type   = TaskType::Blur;
        fullReq.width  = fullW;
        fullReq.height = fullH;
        fullReq.input  = fullImg;

        // Ground truth reference pass
        auto groundTruthRes = localExec.execute(fullReq);

        // Pre-measure warm-up and compute adaptive decision
        for (uint32_t w = 0; w < warmup; ++w) (void)localExec.execute(fullReq);
        auto t0_warm = std::chrono::steady_clock::now();
        auto localWarmRes = localExec.execute(fullReq);
        auto t1_warm = std::chrono::steady_clock::now();
        double localGpuEstUs = std::chrono::duration<double, std::micro>(t1_warm - t0_warm).count();

        TaskResult remoteWarmRes;
        double remoteGpuEstUs = localGpuEstUs * 1.1;
        if (remoteAvailable) {
            for (uint32_t w = 0; w < warmup; ++w) (void)remoteClient.submit(fullReq);
            remoteWarmRes = remoteClient.submit(fullReq);
            remoteGpuEstUs = static_cast<double>(remoteWarmRes.totalUs > 0 ? remoteWarmRes.totalUs : localGpuEstUs * 1.1);
        }

        auto remoteTransfer = remoteClient.lastTransferMetrics();
        float effCompRatioSend = (remoteTransfer.compressedSendBytes > 0)
                                     ? remoteTransfer.compressionRatioSend
                                     : (remoteClient.isCompressionEnabled() ? 3.5f : 1.0f);
        float effCompRatioRecv = (remoteTransfer.compressedRecvBytes > 0)
                                     ? remoteTransfer.compressionRatioRecv
                                     : (remoteClient.isCompressionEnabled() ? 3.5f : 1.0f);
        double compCpuPerByte = (remoteTransfer.uncompressedSendBytes > 0)
                                    ? (static_cast<double>(remoteTransfer.compressionUs) / remoteTransfer.uncompressedSendBytes)
                                    : 0.002;
        double decompCpuPerByte = (remoteTransfer.uncompressedRecvBytes > 0)
                                      ? (static_cast<double>(remoteTransfer.decompressionUs) / remoteTransfer.uncompressedRecvBytes)
                                      : 0.0006;
        const uint32_t apronRadius = 9;

        auto decision = host::WorkloadPartitioner::calculateOptimalDecision(
            fullW, fullH,
            localGpuEstUs,
            remoteGpuEstUs,
            linkBandwidthBps,
            baseRttUs,
            effCompRatioSend,
            300.0,
            500.0,
            effCompRatioRecv,
            apronRadius,
            compCpuPerByte,
            decompCpuPerByte
        );

        std::vector<double> mode1Samples(repetitions, 0.0);
        std::vector<double> mode2Samples(repetitions, 0.0);
        std::vector<double> mode3Samples(repetitions, 0.0);
        std::vector<double> mode4aSamples(repetitions, 0.0);
        std::vector<double> mode4bSamples(repetitions, 0.0);

        TaskResult lastM1, lastM2;
        host::WorkloadPartitioner::PartitionResult lastM3, lastM4a, lastM4b;

        auto runMode1 = [&](uint32_t r) {
            fullReq.taskId = 8000 + r * 10 + 1;
            auto tStart = std::chrono::steady_clock::now();
            lastM1 = localExec.execute(fullReq);
            auto tEnd = std::chrono::steady_clock::now();
            mode1Samples[r] = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        };

        auto runMode2 = [&](uint32_t r) {
            if (!remoteAvailable) { mode2Samples[r] = mode1Samples[r]; return; }
            fullReq.taskId = 8000 + r * 10 + 2;
            auto tStart = std::chrono::steady_clock::now();
            lastM2 = remoteClient.submit(fullReq);
            auto tEnd = std::chrono::steady_clock::now();
            mode2Samples[r] = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        };

        auto runMode3 = [&](uint32_t r) {
            if (!remoteAvailable) { mode3Samples[r] = mode1Samples[r]; return; }
            fullReq.taskId = 8000 + r * 10 + 3;
            auto tStart = std::chrono::steady_clock::now();
            lastM3 = host::WorkloadPartitioner::executeParallelSlabs(
                fullReq, localExecFn, remoteExecFn, 0.50f, apronRadius);
            auto tEnd = std::chrono::steady_clock::now();
            mode3Samples[r] = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        };

        auto runMode4a = [&](uint32_t r) {
            if (!remoteAvailable) { mode4aSamples[r] = mode1Samples[r]; return; }
            fullReq.taskId = 8000 + r * 10 + 4;
            auto tStart = std::chrono::steady_clock::now();
            lastM4a = host::WorkloadPartitioner::executeParallelSlabs(
                fullReq, localExecFn, remoteExecFn, decision.bestCandidateRatio, apronRadius);
            auto tEnd = std::chrono::steady_clock::now();
            mode4aSamples[r] = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        };

        auto runMode4b = [&](uint32_t r) {
            if (!remoteAvailable) { mode4bSamples[r] = mode1Samples[r]; return; }
            fullReq.taskId = 8000 + r * 10 + 5;
            auto tStart = std::chrono::steady_clock::now();
            lastM4b = host::WorkloadPartitioner::executeParallelSlabs(
                fullReq, localExecFn, remoteExecFn, decision.splitRatio, apronRadius);
            auto tEnd = std::chrono::steady_clock::now();
            mode4bSamples[r] = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        };

        // ── Executing in Interleaved Round-Robin Order ────────────────────────
        std::cout << "  Executing interleaved round-robin runs...\n";
        for (uint32_t r = 0; r < repetitions; ++r) {
            // Cyclically shift the execution order for each repetition
            // to eliminate any first-run boost clock or thermal throttling advantage
            uint32_t orderShift = r % 5;
            for (uint32_t step = 0; step < 5; ++step) {
                uint32_t mode = (orderShift + step) % 5;
                switch (mode) {
                    case 0: runMode1(r); break;
                    case 1: runMode2(r); break;
                    case 2: runMode3(r); break;
                    case 3: runMode4a(r); break;
                    case 4: runMode4b(r); break;
                }
            }
        }

        // Compute Benchmark Statistics
        auto statsMode1 = BenchmarkStats::compute(mode1Samples);
        auto statsMode2 = BenchmarkStats::compute(mode2Samples);
        auto statsMode3 = BenchmarkStats::compute(mode3Samples);
        auto statsMode4a = BenchmarkStats::compute(mode4aSamples);
        auto statsMode4b = BenchmarkStats::compute(mode4bSamples);

        double maeM2 = remoteAvailable ? host::FrameCompositor::computeMAE(groundTruthRes.output, lastM2.output) : 0.0;
        double maeM3 = remoteAvailable ? host::FrameCompositor::computeMAE(groundTruthRes.output, lastM3.output) : 0.0;
        double maeM4a = remoteAvailable ? host::FrameCompositor::computeMAE(groundTruthRes.output, lastM4a.output) : 0.0;
        double maeM4b = remoteAvailable ? host::FrameCompositor::computeMAE(groundTruthRes.output, lastM4b.output) : 0.0;

        double fastestSingleGpuMedianMs = std::min(statsMode1.medianMs, statsMode2.medianMs);
        std::string fastestGpuName = (statsMode1.medianMs <= statsMode2.medianMs) ? "RTX 3050" : "GTX 1650 Ti";

        // Print Summary Comparative Table
        std::cout << "\n┌──────────────────────────────────────────┬────────────┬────────────┬────────────┬────────────┬─────────────┬──────────┐\n";
        std::cout << "│ Execution Mode (Interleaved Testing)     │ Median(ms) │ Mean(ms)   │ P95(ms)    │ StdDev(ms) │ vs Best GPU │ Seam MAE │\n";
        std::cout << "├──────────────────────────────────────────┼────────────┼────────────┼────────────┼────────────┼─────────────┼──────────┤\n";
        std::cout << std::format("│ Mode 1: RTX 3050 Only (Full Frame)       │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │   1.00x (Ref)│    0.00  │\n",
                                 statsMode1.medianMs, statsMode1.meanMs, statsMode1.p95Ms, statsMode1.stdDevMs);
        std::cout << std::format("│ Mode 2: GTX 1650 Ti Only (Over Wire)     │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>9.2f}x │ {:>8.2f} │\n",
                                 statsMode2.medianMs, statsMode2.meanMs, statsMode2.p95Ms, statsMode2.stdDevMs,
                                 fastestSingleGpuMedianMs / (statsMode2.medianMs > 0 ? statsMode2.medianMs : 1.0), maeM2);
        std::cout << std::format("│ Mode 3: Both GPUs (Static 50/50 Split)   │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>9.2f}x │ {:>8.2f} │\n",
                                 statsMode3.medianMs, statsMode3.meanMs, statsMode3.p95Ms, statsMode3.stdDevMs,
                                 fastestSingleGpuMedianMs / (statsMode3.medianMs > 0 ? statsMode3.medianMs : 1.0), maeM3);
        std::cout << std::format("│ Mode 4a: Both GPUs (Candidate {:.0f}/{:.0f} Split)│ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>9.2f}x │ {:>8.2f} │\n",
                                 decision.bestCandidateRatio * 100.0f, (1.0f - decision.bestCandidateRatio) * 100.0f,
                                 statsMode4a.medianMs, statsMode4a.meanMs, statsMode4a.p95Ms, statsMode4a.stdDevMs,
                                 fastestSingleGpuMedianMs / (statsMode4a.medianMs > 0 ? statsMode4a.medianMs : 1.0), maeM4a);
        std::string mode4bLabel = decision.isLocalFallback ? "Mode 4b: Both GPUs (Local Fallback)    "
                                                           : "Mode 4b: Both GPUs (Cooperative Split) ";
        std::cout << std::format("│ {} │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>8.2f} ms │ {:>9.2f}x │ {:>8.2f} │\n",
                                 mode4bLabel,
                                 statsMode4b.medianMs, statsMode4b.meanMs, statsMode4b.p95Ms, statsMode4b.stdDevMs,
                                 fastestSingleGpuMedianMs / (statsMode4b.medianMs > 0 ? statsMode4b.medianMs : 1.0), maeM4b);
        std::cout << "└──────────────────────────────────────────┴────────────┴────────────┴────────────┴────────────┴─────────────┴──────────┘\n";

        // Perform Paired Statistical Significance Testing
        auto pairedMode4bVsMode1 = host::StatisticalValidator::computePairedStats(mode1Samples, mode4bSamples, maeM4b);
        auto pairedMode4aVsMode1 = host::StatisticalValidator::computePairedStats(mode1Samples, mode4aSamples, maeM4a);

        host::StatisticalValidator::printPairedReport(
            std::format("{}x{} Paired Test (Cooperative vs Host Alone)", fullW, fullH),
            "RTX 3050 Host Alone",
            decision.isLocalFallback ? "Mode 4b Local Fallback" : "Mode 4b Cooperative Split",
            pairedMode4bVsMode1
        );

        if (!decision.isLocalFallback) {
            host::StatisticalValidator::printPairedReport(
                std::format("{}x{} Candidate Split vs Host Alone", fullW, fullH),
                "RTX 3050 Host Alone",
                std::format("Candidate {:.0f}/{:.0f} Split", decision.bestCandidateRatio * 100.0f, (1.0f - decision.bestCandidateRatio) * 100.0f),
                pairedMode4aVsMode1
            );
        }

        // Add to JSON Export
        std::string jsonSec = std::format(
            "    {{\n"
            "      \"resolution\": \"{}x{}\",\n"
            "      \"repetitions\": {},\n"
            "      \"scene_type\": \"{}\",\n"
            "      \"mode1_host_alone_median_ms\": {:.2f},\n"
            "      \"mode2_remote_alone_median_ms\": {:.2f},\n"
            "      \"mode3_static_5050_median_ms\": {:.2f},\n"
            "      \"mode4a_candidate_median_ms\": {:.2f},\n"
            "      \"mode4b_policy_median_ms\": {:.2f},\n"
            "      \"paired_mean_delta_ms\": {:.2f},\n"
            "      \"ci95_lower_ms\": {:.2f},\n"
            "      \"ci95_upper_ms\": {:.2f},\n"
            "      \"p_value\": {:.4f},\n"
            "      \"speedup_vs_best\": {:.2f},\n"
            "      \"statistically_significant\": {},\n"
            "      \"seam_mae\": {:.2f}\n"
            "    }}",
            fullW, fullH, repetitions, (sceneType == 1 ? "noise" : "realistic_game"),
            statsMode1.medianMs, statsMode2.medianMs, statsMode3.medianMs,
            statsMode4a.medianMs, statsMode4b.medianMs,
            pairedMode4bVsMode1.meanDeltaMs, pairedMode4bVsMode1.ci95LowerMs, pairedMode4bVsMode1.ci95UpperMs,
            pairedMode4bVsMode1.pValue, pairedMode4bVsMode1.speedupRatio,
            pairedMode4bVsMode1.isStatisticallySignificant ? "true" : "false",
            maeM4b
        );
        g_jsonExport.addSection(jsonSec);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Part C: Wire Compression & Software LZ4 Codec Validation (Priority 3)
    // ──────────────────────────────────────────────────────────────────────────
    if (remoteAvailable) {
        std::cout << "\n╔══════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║ Part C: Wire Compression & Software LZ4 Codec Validation Benchmark   ║\n";
        std::cout << "║ Comparing Uncompressed vs Software LZ4 CPU Codec across Image Types  ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";
        std::cout << "  Note: Codec is High-throughput Software LZ4 CPU Block Codec (CPU software, not hardware-accelerated).\n";

        const uint32_t compTestW = 1024;
        const uint32_t compTestH = 1024;

        struct TextureTestCase {
            const char* name;
            int type;
        };
        const std::array<TextureTestCase, 2> testCases = {{
            {"Procedural Realistic Game Frame (High Compressibility)", 0},
            {"Procedural High-Frequency Noise (Low Compressibility)", 1}
        }};

        for (const auto& tc : testCases) {
            std::cout << std::format("\n  --- Texture: {} ({}x{}) ---\n", tc.name, compTestW, compTestH);
            auto img = renderHostScene(compTestW, compTestH, 99, tc.type);

            TaskRequest req;
            req.taskId = 9000;
            req.type   = TaskType::Blur;
            req.width  = compTestW;
            req.height = compTestH;
            req.input  = img;

            // 1. Uncompressed Wire Transfer
            remoteClient.setCompressionEnabled(false);
            for (uint32_t w = 0; w < warmup; ++w) (void)remoteClient.submit(req);
            std::vector<double> uncompSamples;
            uncompSamples.reserve(repetitions);
            for (uint32_t r = 0; r < repetitions; ++r) {
                req.taskId = 9100 + r;
                auto t0 = std::chrono::steady_clock::now();
                (void)remoteClient.submit(req);
                auto t1 = std::chrono::steady_clock::now();
                uncompSamples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            auto uncompStats = BenchmarkStats::compute(uncompSamples);
            auto uncompTransfer = remoteClient.lastTransferMetrics();

            // 2. Software LZ4 CPU Compressed Transfer
            remoteClient.setCompressionEnabled(true);
            for (uint32_t w = 0; w < warmup; ++w) (void)remoteClient.submit(req);
            std::vector<double> compSamples;
            compSamples.reserve(repetitions);
            for (uint32_t r = 0; r < repetitions; ++r) {
                req.taskId = 9200 + r;
                auto t0 = std::chrono::steady_clock::now();
                (void)remoteClient.submit(req);
                auto t1 = std::chrono::steady_clock::now();
                compSamples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            auto compStats = BenchmarkStats::compute(compSamples);
            auto compTransfer = remoteClient.lastTransferMetrics();

            // Dynamic compression prediction: only compress if compressUs + wireSend(comp) < wireSend(uncomp)
            double wireTimeUncompUs = (uncompTransfer.uncompressedSendBytes / static_cast<double>(linkBandwidthBps)) * 1e6;
            double wireTimeCompUs   = (compTransfer.compressedSendBytes / static_cast<double>(linkBandwidthBps)) * 1e6;
            double compOverheadUs   = static_cast<double>(compTransfer.compressionUs);
            bool predictedBenefit   = (compOverheadUs + wireTimeCompUs < wireTimeUncompUs);

            std::cout << "\n  ┌───────────────────────────────────┬──────────────┬──────────────┬──────────────┬──────────────┐\n";
            std::cout << "  │ Metric                            │ Uncompressed │ LZ4 Compress │ Unit / Ratio │ Net Benefit  │\n";
            std::cout << "  ├───────────────────────────────────┼──────────────┼──────────────┼──────────────┼──────────────┤\n";
            std::cout << std::format("  │ Payload Send Bytes                │ {:>10.2f} KB│ {:>10.2f} KB│ {:>10.2f}x   │ {:>+10.2f} KB│\n",
                                     uncompTransfer.uncompressedSendBytes / 1024.0, compTransfer.compressedSendBytes / 1024.0,
                                     compTransfer.compressionRatioSend,
                                     (static_cast<double>(compTransfer.compressedSendBytes) - uncompTransfer.uncompressedSendBytes) / 1024.0);
            std::cout << std::format("  │ Payload Recv Bytes                │ {:>10.2f} KB│ {:>10.2f} KB│ {:>10.2f}x   │ {:>+10.2f} KB│\n",
                                     uncompTransfer.uncompressedRecvBytes / 1024.0, compTransfer.compressedRecvBytes / 1024.0,
                                     compTransfer.compressionRatioRecv,
                                     (static_cast<double>(compTransfer.compressedRecvBytes) - uncompTransfer.uncompressedRecvBytes) / 1024.0);
            std::cout << std::format("  │ Host CPU Compression Time         │         0 us │ {:>10} us │   microsec   │              │\n",
                                     compTransfer.compressionUs);
            std::cout << std::format("  │ Host CPU Decompression Time       │         0 us │ {:>10} us │   microsec   │              │\n",
                                     compTransfer.decompressionUs);
            std::cout << std::format("  │ End-to-End Latency (Median)       │ {:>10.2f} ms│ {:>10.2f} ms│ {:>10.2f}x   │ {:>+10.2f} ms│\n",
                                     uncompStats.medianMs, compStats.medianMs,
                                     uncompStats.medianMs / compStats.medianMs,
                                     uncompStats.medianMs - compStats.medianMs);
            std::cout << std::format("  │ End-to-End Latency (P95)          │ {:>10.2f} ms│ {:>10.2f} ms│ {:>10.2f}x   │ {:>+10.2f} ms│\n",
                                     uncompStats.p95Ms, compStats.p95Ms,
                                     uncompStats.p95Ms / compStats.p95Ms,
                                     uncompStats.p95Ms - compStats.p95Ms);
            std::cout << "  └───────────────────────────────────┴──────────────┴──────────────┴──────────────┴──────────────┘\n";
            std::cout << std::format("    -> Dynamic Policy Prediction: {}\n",
                                     predictedBenefit ? "COMPRESSION RECOMMENDED (CPU codec overhead is outweighed by wire time savings)"
                                                      : "COMPRESSION BYPASSED (Data is incompressible; CPU codec overhead exceeds bandwidth savings)");
        }
        // Restore default compression setting
        remoteClient.setCompressionEnabled(true);
    }
}

// ─── Entry Point ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::cout << "=================================================================\n";
    std::cout << " DistributedGPU Host Application — M4/M5 Cooperative Runtime\n";
    std::cout << " Host: Laptop A (RTX 3050) | Worker: Laptop B (GTX 1650 Ti)\n";
    std::cout << "=================================================================\n";

    std::string workerHost = DEFAULT_WORKER_HOST;
    uint16_t    workerPort = DEFAULT_WORKER_PORT;
    bool        localOnly  = false;
    bool        stage5Only = false;
    bool        enableCompression = true;
    bool        hasPositionalHost = false;
    uint32_t    linkMbps   = 100; // Default 100 Mbps Ethernet link
    uint32_t    linkLatencyMs = 1;
    uint32_t    repetitions = 15;
    uint32_t    warmup = 3;
    int         sceneType = 0; // 0 = gradient, 1 = noisy

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: host.exe [options] [worker_host] [worker_port]\n\n"
                      << "Options:\n"
                      << "  -h, --help           Show this help message and exit\n"
                      << "  --host <ip>          Remote worker IP address (default: 127.0.0.1)\n"
                      << "  --port <port>        Remote worker port (default: 7777)\n"
                      << "  --link-mbps <mbps>   Network link bandwidth in Mbps (default: 100)\n"
                      << "  --link-latency-ms <ms> Base round-trip network latency in ms (default: 1)\n"
                      << "  --repetitions <n>    Number of benchmark runs per mode (default: 15)\n"
                      << "  --warmup <n>         Number of warm-up runs (default: 3)\n"
                      << "  --image-type <type>  Input texture: 'gradient' or 'noise' (default: gradient)\n"
                      << "  --iterations <n>     Compute shader iterations per pass (sets DGPU_ITERATIONS)\n"
                      << "  --intensity <n>      Kernel radius intensity (sets DGPU_INTENSITY)\n"
                      << "  --compress           Enable LZ4 wire compression (default: enabled)\n"
                      << "  --no-compress        Disable LZ4 wire compression\n"
                      << "  --local-only         Run local benchmarks only without connecting\n"
                      << "  --stage5-only        Run Stage 5 parallel workload benchmarks only\n\n"
                      << "Positional arguments (backward compatible):\n"
                      << "  [worker_host]        Remote worker hostname or IP address\n"
                      << "  [worker_port]        Remote worker TCP port\n\n"
                      << "Examples:\n"
                      << "  host.exe --local-only\n"
                      << "  host.exe --stage5-only 192.168.1.14 7777\n"
                      << "  host.exe --stage5-only --repetitions 30 192.168.1.14 7777\n"
                      << "  host.exe --link-mbps 100 192.168.1.14 7777\n";
            return 0;
        } else if (arg == "--validate") {
            stage5Only = true;
            repetitions = 100;
            std::cout << "[host] --validate requested: executing 100-repetition interleaved statistical validation suite\n";
        } else if (arg == "--local-only") {
            localOnly = true;
        } else if (arg == "--stage5-only") {
            stage5Only = true;
        } else if (arg == "--compress") {
            enableCompression = true;
        } else if (arg == "--no-compress") {
            enableCompression = false;
        } else if (arg == "--repetitions" && i + 1 < argc) {
            repetitions = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--image-type" && i + 1 < argc) {
            std::string t = argv[++i];
            sceneType = (t == "noise" || t == "noisy") ? 1 : 0;
        } else if (arg == "--iterations" && i + 1 < argc) {
            std::string iters = argv[++i];
            SetEnvironmentVariableA("DGPU_ITERATIONS", iters.c_str());
            std::cout << "[host] Set DGPU_ITERATIONS = " << iters << "\n";
        } else if (arg == "--intensity" && i + 1 < argc) {
            std::string intens = argv[++i];
            SetEnvironmentVariableA("DGPU_INTENSITY", intens.c_str());
            std::cout << "[host] Set DGPU_INTENSITY = " << intens << "\n";
        } else if (arg == "--link-mbps" && i + 1 < argc) {
            linkMbps = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--link-latency-ms" && i + 1 < argc) {
            linkLatencyMs = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--host" && i + 1 < argc) {
            workerHost = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            workerPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (!arg.starts_with("-")) {
            if (!hasPositionalHost) {
                workerHost = arg;
                hasPositionalHost = true;
            } else {
                workerPort = static_cast<uint16_t>(std::stoi(arg));
            }
        } else {
            std::cerr << "[host] Unknown option: " << arg << " (use --help for usage)\n";
            return 1;
        }
    }

    dgpu::networking::WinsockInit wsa;
    if (!wsa.ok()) {
        std::cerr << "[host] Winsock initialization failed\n";
        return 1;
    }

    // 1. Initialise local GPU (RTX 3050)
    dgpu::gpu_executor::GpuExecutor localExec;
    std::cout << "[host] Initialising local GPU context (" << PREFERRED_ADAPTER << ")...\n";
    if (!localExec.init(PREFERRED_ADAPTER)) {
        std::cerr << "[host] Local GPU init failed. Ensure D3D12 and NVIDIA drivers are active.\n";
        return 2;
    }
    std::cout << "[host] Local GPU active: " << localExec.adapterInfo().name << "\n";

    // 2. Initialise Frame Compositor
    dgpu::host::FrameCompositor compositor;

    // 3. Connect to Worker (GTX 1650 Ti) and perform protocol handshake
    dgpu::networking::TaskClient remoteClient;
    bool remoteAvailable = false;
    if (localOnly) {
        std::cout << "[host] --local-only mode selected: skipping remote connection\n";
    } else {
        std::cout << "[host] Connecting to remote worker at " << workerHost << ":" << workerPort << "...\n";
        remoteAvailable = remoteClient.connect(workerHost, workerPort);
        if (remoteAvailable) {
            std::cout << "[host] Handshake verified with worker at " << workerHost << ":" << workerPort << '\n';
            if (remoteClient.hasCapabilities()) {
                const auto& caps = remoteClient.capabilities();
                std::cout << "[host] Registered Worker ID: " << caps.workerId << '\n';
                std::cout << "[host] Worker Capabilities: GPU=\"" << caps.gpuName << "\""
                          << " VRAM=" << (caps.vramBytes / (1024 * 1024)) << " MB, "
                          << "Max Resolution=" << caps.maxWidth << "x" << caps.maxHeight << ", "
                          << "Supported Tasks Bitmask=0x" << std::hex << caps.supportedTasks << std::dec << '\n';
            }
            if (!remoteClient.ping()) {
                std::cerr << "[host] Ping failed — switching to local-only mode\n";
                remoteAvailable = false;
            } else {
                remoteClient.setCompressionEnabled(enableCompression);
                std::cout << "[host] Wire compression: " << (enableCompression ? "LZ4 ENABLED" : "DISABLED") << '\n';
            }
        } else {
            std::cout << "[host] Worker not reachable — running local benchmark mode\n";
        }
    }

    // 4. Initialise JobScheduler (wires localExec and remoteClient together)
    dgpu::host::JobScheduler scheduler;
    if (remoteAvailable && remoteClient.hasCapabilities()) {
        scheduler.setWorkerCapabilities(remoteClient.capabilities());
    }

    // Initialise Worker Health Monitor
    std::unique_ptr<dgpu::networking::WorkerHealthMonitor> healthMonitor;
    if (remoteAvailable) {
        healthMonitor = std::make_unique<dgpu::networking::WorkerHealthMonitor>(
            remoteClient, workerHost, workerPort);
        healthMonitor->setStateCallback([](dgpu::networking::WorkerHealthState oldS,
                                           dgpu::networking::WorkerHealthState newS) {
            std::cout << "[host] Health State Transition: "
                      << dgpu::networking::healthStateToString(oldS) << " -> "
                      << dgpu::networking::healthStateToString(newS) << '\n';
        });
        healthMonitor->start();
    }
    scheduler.setLocalExecutor([&localExec](const dgpu::protocol::TaskRequest& req) {
        return localExec.execute(req);
    });
    if (remoteAvailable) {
        scheduler.setRemoteExecutor([&remoteClient](const dgpu::protocol::TaskRequest& req) {
            return remoteClient.submit(req);
        });
    } else {
        scheduler.setRemoteExecutor([&localExec](const dgpu::protocol::TaskRequest& req) {
            return localExec.execute(req);
        });
    }
    scheduler.setLinkBandwidthBps(static_cast<uint64_t>(linkMbps) * 125'000); // 1 Mbps = 125,000 bytes/sec
    scheduler.setBaseRttUs(static_cast<uint64_t>(linkLatencyMs) * 1000);
    scheduler.start(2);
    std::cout << std::format("[host] Analytical scheduler active: Link = {} Mbps, Base RTT = {} ms\n",
                             linkMbps, linkLatencyMs);

    // 5. Run the complete benchmark matrix across all resolutions
    if (!stage5Only) {
        for (const auto& cfg : kBenchConfigs) {
            runDistributedBenchmark(cfg, localExec, remoteClient, scheduler, compositor, remoteAvailable);
        }
    }

    // 5b. Stage 5: Independent Parallel Workloads & Throughput Benchmark
    runStage5ParallelWorkload(localExec, remoteClient, remoteAvailable,
                              static_cast<uint64_t>(linkMbps) * 125'000,
                              static_cast<uint64_t>(linkLatencyMs) * 1000,
                              repetitions, warmup, sceneType);

    // 6. Report Scheduler Metrics (M5)
    const auto& sMetrics = scheduler.metrics();
    std::cout << "\n─────────────────────────────────────────────────────────────────\n";
    std::cout << " JobScheduler & Circuit Breaker Summary (M5)\n";
    std::cout << "─────────────────────────────────────────────────────────────────\n";
    std::cout << std::format("  Local tasks executed:   {}\n", sMetrics.localCount);
    std::cout << std::format("  Remote tasks executed:  {}\n", sMetrics.remoteCount);
    std::cout << std::format("  Circuit fallbacks:      {}\n", sMetrics.fallbackCount);
    std::cout << std::format("  EMA Local GPU:          {:.0f} us\n", sMetrics.emaLocalUs);
    std::cout << std::format("  EMA Remote GPU:         {:.0f} us\n", sMetrics.emaRemoteUs);
    std::cout << std::format("  EMA Network RTT:        {:.0f} us\n", sMetrics.emaNetworkUs);
    std::cout << std::format("  Circuit Breaker state:  {}\n",
        scheduler.circuitState() == dgpu::task_runtime::CircuitBreaker::State::Closed ? "CLOSED (Healthy)" :
        scheduler.circuitState() == dgpu::task_runtime::CircuitBreaker::State::Open   ? "OPEN (Tripped)" : "HALF-OPEN");

    // 7. Export full Telemetry to CSV
    auto csvPath = std::filesystem::path("host_benchmark.csv");
    if (dgpu::telemetry::Telemetry::instance().exportCsv(csvPath)) {
        std::cout << "[host] Full frame telemetry exported to " << csvPath.string() << '\n';
    }

    // 7b. Export Structured Benchmark Results to JSON
    g_jsonExport.testName = "DistributedGPU M4/M5 Cooperative Multi-GPU Benchmark";
    g_jsonExport.hardwareHost = localExec.adapterInfo().name;
    g_jsonExport.hardwareWorker = remoteAvailable ? "GTX 1650 Ti" : "None";
    g_jsonExport.osName = "Windows 11 x64";
    g_jsonExport.linkMbps = linkMbps;
    g_jsonExport.linkLatencyMs = linkLatencyMs;
    auto jsonPath = std::filesystem::path("benchmark_results.json");
    g_jsonExport.writeToFile(jsonPath);
    std::cout << "[host] Structured statistical benchmark results exported to " << jsonPath.string() << '\n';

    // 8. Print formatted summary comparison table
    std::cout << "\n┌────────────────────────────────────┬──────────┬──────────┬──────────┐\n";
    std::cout << "│ Mode / Resolution                  │ Mean(us) │ P95(us)  │ FPS      │\n";
    std::cout << "├────────────────────────────────────┼──────────┼──────────┼──────────┤\n";
    for (const auto& cfg : kBenchConfigs) {
        for (const char* prefix : {"local", "remote_sync", "remote_async", "adaptive"}) {
            auto label = std::format("{}_{}x{}", prefix, cfg.width, cfg.height);
            auto rep   = dgpu::telemetry::Telemetry::instance().summarise(label);
            if (rep.sampleCount == 0) continue;
            std::cout << std::format("│ {:<34} │ {:>8.0f} │ {:>8.0f} │ {:>8.1f} │\n",
                                     label, rep.meanUs, rep.p95Us, rep.avgFps);
        }
    }
    std::cout << "└────────────────────────────────────┴──────────┴──────────┴──────────┘\n";

    if (healthMonitor) healthMonitor->stop();
    scheduler.stop();
    if (remoteAvailable) remoteClient.disconnect();

    std::cout << "[host] Distributed rendering benchmark finished successfully.\n";
    return 0;
}
