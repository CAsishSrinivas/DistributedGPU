// apps/worker/worker_gpu_executor.hpp
// Worker-app-specific wrapper around libs/d3d12/GpuExecutor.
#pragma once

#include "d3d12/d3d12_context.hpp"
#include "protocol/message.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace dgpu::worker {

class WorkerGpuExecutor {
public:
    WorkerGpuExecutor();
    ~WorkerGpuExecutor();

    WorkerGpuExecutor(const WorkerGpuExecutor&)            = delete;
    WorkerGpuExecutor& operator=(const WorkerGpuExecutor&) = delete;

    [[nodiscard]] bool init(const std::string& preferredAdapter = "GTX 1650");

    [[nodiscard]] protocol::TaskResult execute(const protocol::TaskRequest& req);

    [[nodiscard]] const d3d12::AdapterInfo& adapterInfo() const noexcept;

    // Counters for telemetry / heartbeat status
    [[nodiscard]] uint64_t completedCount() const noexcept;
    [[nodiscard]] uint64_t totalGpuUs()     const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::worker
