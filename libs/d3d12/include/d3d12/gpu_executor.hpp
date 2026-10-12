// gpu_executor/gpu_executor.hpp
// Executes registered GPU tasks on the local D3D12 device (M3).
#pragma once

#include "d3d12/d3d12_context.hpp"
#include "protocol/message.hpp"

#include <memory>
#include <string>

namespace dgpu::gpu_executor {

// ─── GpuExecutor ─────────────────────────────────────────────────────────────
// Owns a D3D12Context and a pool of reusable GPU buffers.
// Each call to execute() is synchronous and GPU-exclusive.

class GpuExecutor {
public:
    GpuExecutor();
    ~GpuExecutor();

    GpuExecutor(const GpuExecutor&)            = delete;
    GpuExecutor& operator=(const GpuExecutor&) = delete;

    // Initialise the D3D12 device and pre-compile shaders.
    // preferredAdapter: substring matched against adapter name (e.g. "GTX 1650")
    [[nodiscard]] bool init(const std::string& preferredAdapter = "");

    // Execute a task and return the result with timing breakdowns.
    [[nodiscard]] protocol::TaskResult execute(const protocol::TaskRequest& req);

    [[nodiscard]] const d3d12::AdapterInfo& adapterInfo() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::gpu_executor
