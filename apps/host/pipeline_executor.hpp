// apps/host/pipeline_executor.hpp
// Pipelined transfer, compression, and GPU execution engine (Phase 2.3).
// Overlaps:
//   Stage 1: CPU LZ4 Block Compression / Input Preprocessing
//   Stage 2: Asynchronous Wire Transfer / Device Staging with bounded in-flight queue
//   Stage 3: Local/Remote GPU Compute Dispatch
//   Stage 4: Readback & CPU LZ4 Block Decompression / Postprocessing
#pragma once

#include "protocol/message.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dgpu::host {

// ─── Pipeline Configuration ──────────────────────────────────────────────────

struct PipelineConfig {
    uint32_t inFlightDepth{2};              // Bounded in-flight queue depth (double-buffering = 2)
    bool     overlapCompression{true};      // Overlap LZ4 CPU compression with execution
    bool     overlapDecompression{true};    // Overlap LZ4 CPU decompression with next execution
};

// ─── Pipeline Stage Performance Metrics ──────────────────────────────────────

struct PipelineStageMetrics {
    uint32_t totalTasks{0};
    uint64_t totalWallClockUs{0};
    uint64_t totalCompressionUs{0};
    uint64_t totalTransferWireUs{0};
    uint64_t totalGpuComputeUs{0};
    uint64_t totalDecompressionUs{0};
    double   throughputTasksPerSec{0.0};
    double   speedupVsSequential{1.0};
    double   concurrencyFactor{1.0};        // sum(stage durations) / totalWallClock
};

// ─── PipelineExecutor ────────────────────────────────────────────────────────

class PipelineExecutor {
public:
    using LocalExecFn  = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;
    using RemoteExecFn = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;

    explicit PipelineExecutor(PipelineConfig config = {});
    ~PipelineExecutor();

    PipelineExecutor(const PipelineExecutor&)            = delete;
    PipelineExecutor& operator=(const PipelineExecutor&) = delete;

    // Executes a batch of tasks through the 4-stage asynchronous overlap pipeline
    [[nodiscard]] std::vector<protocol::TaskResult> executePipeline(
        const std::vector<protocol::TaskRequest>& tasks,
        LocalExecFn localExec,
        RemoteExecFn remoteExec,
        bool remoteAvailable,
        PipelineStageMetrics* outMetrics = nullptr);

    [[nodiscard]] const PipelineConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::host
