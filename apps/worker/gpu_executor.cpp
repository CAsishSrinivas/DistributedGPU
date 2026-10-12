// apps/worker/gpu_executor.cpp
// Worker-side GPU executor: thin wrapper that adapts the libs/d3d12 GpuExecutor
// to the worker app's needs (adapter verification, per-task logging).
//
// The real execution logic lives in libs/d3d12/src/gpu_executor.cpp.
// This file exists so the worker app has a place to add worker-specific
// concerns (e.g. VRAM pressure checks, per-task telemetry hooks) without
// touching the shared library.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "worker_gpu_executor.hpp"
#include "d3d12/gpu_executor.hpp"
#include "telemetry/telemetry.hpp"

#include <format>
#include <iostream>

namespace dgpu::worker {

// ─── WorkerGpuExecutor::Impl ─────────────────────────────────────────────────

struct WorkerGpuExecutor::Impl {
    dgpu::gpu_executor::GpuExecutor exec;
    std::atomic<uint64_t>           completedCount{0};
    std::atomic<uint64_t>           totalGpuUs{0};
};

WorkerGpuExecutor::WorkerGpuExecutor()  : m_impl(std::make_unique<Impl>()) {}
WorkerGpuExecutor::~WorkerGpuExecutor() = default;

bool WorkerGpuExecutor::init(const std::string& preferredAdapter) {
    if (!m_impl->exec.init(preferredAdapter)) return false;

    // Verify we're running on the expected hardware
    const auto& info = m_impl->exec.adapterInfo();
    std::cout << std::format("[worker_gpu] Initialised on: {} ({} MB VRAM)\n",
                             info.name,
                             info.dedicatedVideoMemoryBytes / (1024 * 1024));

    // Warn if the adapter doesn't match the expected GTX 1650 Ti
    if (preferredAdapter.empty()) return true;
    if (info.name.find(preferredAdapter) == std::string::npos) {
        std::cerr << std::format(
            "[worker_gpu] WARNING: expected adapter containing '{}' "
            "but got '{}'\n", preferredAdapter, info.name);
    }
    return true;
}

protocol::TaskResult WorkerGpuExecutor::execute(const protocol::TaskRequest& req) {
    auto result = m_impl->exec.execute(req);

    if (result.status == protocol::TaskStatus::Ok) {
        ++m_impl->completedCount;
        m_impl->totalGpuUs += result.gpuUs;

        // Record worker-side telemetry
        telemetry::FrameRecord fr;
        fr.frameIndex  = m_impl->completedCount.load();
        fr.label       = std::format("worker_{}x{}", req.width, req.height);
        fr.isRemote    = false; // from this process, it's local
        fr.width       = req.width;
        fr.height      = req.height;
        fr.uploadUs    = result.uploadUs;
        fr.gpuExecUs   = result.gpuUs;
        fr.readbackUs  = result.readbackUs;
        fr.totalUs     = result.totalUs;
        telemetry::Telemetry::instance().record(fr);
    }

    return result;
}

const d3d12::AdapterInfo& WorkerGpuExecutor::adapterInfo() const noexcept {
    return m_impl->exec.adapterInfo();
}

uint64_t WorkerGpuExecutor::completedCount() const noexcept {
    return m_impl->completedCount.load();
}

uint64_t WorkerGpuExecutor::totalGpuUs() const noexcept {
    return m_impl->totalGpuUs.load();
}

} // namespace dgpu::worker
