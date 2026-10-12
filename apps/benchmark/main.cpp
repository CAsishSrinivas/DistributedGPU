// apps/benchmark/main.cpp
// Standalone M0/M1 validator — no network required.
// Runs the compute shader locally and reports pass/fail + timings.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "d3d12/d3d12_context.hpp"
#include "d3d12/gpu_executor.hpp"
#include "telemetry/telemetry.hpp"

#include <array>
#include <cstdio>
#include <format>
#include <iostream>
#include <numeric>
#include <vector>

// ─── Acceptance test helpers ──────────────────────────────────────────────────

// Pixel-wise mean absolute error must be below this threshold for the
// deterministic blur test to pass.
static constexpr double kMaxMAE = 5.0; // out of 255

static double computeMAE(const std::vector<std::byte>& a,
                          const std::vector<std::byte>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return sum / static_cast<double>(a.size());
}

// ─── Synthetic image ──────────────────────────────────────────────────────────

static std::vector<std::byte> makeGradient(uint32_t w, uint32_t h) {
    std::vector<std::byte> img(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            img[i+0] = static_cast<std::byte>((x * 255u) / w);
            img[i+1] = static_cast<std::byte>((y * 255u) / h);
            img[i+2] = static_cast<std::byte>(128);
            img[i+3] = static_cast<std::byte>(255);
        }
    }
    return img;
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== DistributedGPU Local Benchmark (M0/M1) ===\n\n";

    // Enumerate adapters (M0 acceptance)
    auto adapters = dgpu::d3d12::enumerateAdapters();
    std::cout << "Detected GPU adapters:\n";
    for (const auto& a : adapters) {
        std::cout << std::format("  [{}] {} — {} MB VRAM{}\n",
                                 a.index, a.name,
                                 a.dedicatedVideoMemoryBytes / (1024 * 1024),
                                 a.isSoftware ? " (software)" : "");
    }
    if (adapters.empty()) {
        std::cerr << "\nFAIL: No GPU adapters found.\n";
        return 1;
    }

    // Initialise executor
    dgpu::gpu_executor::GpuExecutor exec;
    if (!exec.init()) {
        std::cerr << "\nFAIL: GPU executor init failed.\n";
        return 2;
    }
    std::cout << "\nUsing: " << exec.adapterInfo().name << "\n\n";

    // ── M1 benchmark matrix ───────────────────────────────────────────────────
    struct Run { uint32_t w, h; const char* label; };
    constexpr std::array<Run, 4> kRuns{{
        {  256,  256, "blur_256x256"  },
        {  512,  512, "blur_512x512"  },
        { 1024, 1024, "blur_1024x1024"},
        { 1920, 1080, "blur_1920x1080"},
    }};

    static constexpr uint32_t WARMUP = 3;
    static constexpr uint32_t RUNS   = 20;

    bool allPassed = true;
    uint64_t taskId = 1;

    for (const auto& run : kRuns) {
        auto image = makeGradient(run.w, run.h);
        std::cout << std::format("── {} ──\n", run.label);

        std::vector<uint64_t> totalTimes;
        std::vector<std::byte> firstOutput;

        for (uint32_t i = 0; i < WARMUP + RUNS; ++i) {
            dgpu::protocol::TaskRequest req;
            req.taskId = taskId++;
            req.type   = dgpu::protocol::TaskType::Blur;
            req.width  = run.w;
            req.height = run.h;
            req.input  = image;

            auto result = exec.execute(req);

            if (result.status != dgpu::protocol::TaskStatus::Ok) {
                std::cerr << "  FAIL: GPU returned error status\n";
                allPassed = false;
                break;
            }

            if (i == WARMUP) firstOutput = result.output; // save for correctness check

            if (i < WARMUP) continue;

            // Verify output size
            if (result.output.size() != image.size()) {
                std::cerr << "  FAIL: output size mismatch\n";
                allPassed = false;
            }

            totalTimes.push_back(result.totalUs);

            dgpu::telemetry::FrameRecord fr;
            fr.frameIndex  = i - WARMUP;
            fr.label       = run.label;
            fr.isRemote    = false;
            fr.width       = run.w;
            fr.height      = run.h;
            fr.uploadUs    = result.uploadUs;
            fr.gpuExecUs   = result.gpuUs;
            fr.readbackUs  = result.readbackUs;
            fr.totalUs     = result.totalUs;
            dgpu::telemetry::Telemetry::instance().record(fr);
        }

        if (!totalTimes.empty()) {
            auto rep = dgpu::telemetry::Telemetry::instance().summarise(run.label);
            // Correctness: two runs should produce the same output
            auto image2 = makeGradient(run.w, run.h);
            dgpu::protocol::TaskRequest req2;
            req2.taskId = taskId++;
            req2.type   = dgpu::protocol::TaskType::Blur;
            req2.width  = run.w;
            req2.height = run.h;
            req2.input  = image2;
            auto res2   = exec.execute(req2);
            double mae  = computeMAE(firstOutput, res2.output);

            bool pass   = (mae <= kMaxMAE);
            allPassed  &= pass;

            std::cout << std::format("  mean={:.0f}us  p95={:.0f}us  fps={:.1f}  MAE={:.2f}  {}\n",
                                     rep.meanUs, rep.p95Us, rep.avgFps, mae,
                                     pass ? "PASS" : "FAIL");
        }
    }

    // Save CSV
    auto csvPath = std::filesystem::path("benchmark_results.csv");
    dgpu::telemetry::Telemetry::instance().exportCsv(csvPath);
    std::cout << "\nResults saved to " << csvPath.string() << '\n';

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
