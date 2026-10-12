// d3d12/pipelined_gpu_executor.hpp
// Multi-buffered / double-buffered D3D12 GPU compute executor (Phase 2.3).
// Overlaps CPU upload / command list recording with GPU compute execution
// and asynchronous readback using timeline fence synchronization.
#pragma once

#include "d3d12/d3d12_context.hpp"
#include "protocol/message.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dgpu::gpu_executor {

// ─── Metrics collected during pipelined execution ────────────────────────────

struct PipelinedExecutionMetrics {
    uint32_t totalTasks{0};
    uint32_t slotCount{2};
    uint64_t totalWallClockUs{0};
    uint64_t sumGpuComputeUs{0};
    uint64_t sumUploadUs{0};
    uint64_t sumReadbackUs{0};
    double   throughputTasksPerSec{0.0};
    double   speedupVsSequential{1.0};
    double   overlapRatio{0.0}; // (sumStageUs - totalWallClockUs) / sumStageUs
};

// ─── PipelinedGpuExecutor ───────────────────────────────────────────────────
// Manages N independent staging slots (double-buffered by default),
// each with dedicated upload/readback buffers, command allocator, and command list.
// Fences synchronize execution safely between CPU and GPU without pipeline stalls.

class PipelinedGpuExecutor {
public:
    PipelinedGpuExecutor();
    ~PipelinedGpuExecutor();

    PipelinedGpuExecutor(const PipelinedGpuExecutor&)            = delete;
    PipelinedGpuExecutor& operator=(const PipelinedGpuExecutor&) = delete;

    // Initialise device, compile compute shaders, and pre-allocate staging slots.
    [[nodiscard]] bool init(const std::string& preferredAdapter = "", uint32_t slotCount = 2);

    // Asynchronously submits a task into a specific slot without blocking on GPU completion.
    // Uploads input to slot upload heap, records command list, and executes on command queue.
    [[nodiscard]] bool submitAsync(const protocol::TaskRequest& req, uint32_t slotIndex);

    // Blocks until the specified slot's GPU computation finishes and copies output back to CPU.
    [[nodiscard]] protocol::TaskResult collect(uint32_t slotIndex);

    // Synchronous execution fallback for a single task.
    [[nodiscard]] protocol::TaskResult execute(const protocol::TaskRequest& req);

    // Batch execution using double/multi-buffering to achieve concurrent CPU/GPU overlap.
    [[nodiscard]] std::vector<protocol::TaskResult> executePipelined(
        const std::vector<protocol::TaskRequest>& tasks,
        PipelinedExecutionMetrics* outMetrics = nullptr);

    [[nodiscard]] uint32_t slotCount() const noexcept;
    [[nodiscard]] const d3d12::AdapterInfo& adapterInfo() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::gpu_executor
