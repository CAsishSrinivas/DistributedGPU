// libs/d3d12/src/pipelined_gpu_executor.cpp
// Multi-buffered / double-buffered D3D12 GPU compute executor (Phase 2.3).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "d3d12/pipelined_gpu_executor.hpp"
#include "d3d12/d3d12_context.hpp"
#include "shader_sources.hpp"
#include "telemetry/telemetry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dgpu::gpu_executor {

static void transitionResource(ID3D12GraphicsCommandList* cl,
                               ID3D12Resource* res,
                               D3D12_RESOURCE_STATES& curState,
                               D3D12_RESOURCE_STATES targetState) {
    if (curState == targetState) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = curState;
    b.Transition.StateAfter  = targetState;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    curState = targetState;
}

// ─── Slot: Dedicated resources for one stage of the pipeline ─────────────────

struct PipelineSlot {
    d3d12::GpuBuffer upload;
    d3d12::GpuBuffer gpuInput;
    d3d12::GpuBuffer gpuOutput;
    d3d12::GpuBuffer readback;

    ComPtr<ID3D12CommandAllocator>     cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList4> cmdList;

    uint32_t width{0};
    uint32_t height{0};
    bool     valid{false};
    bool     inFlight{false};

    D3D12_RESOURCE_STATES gpuInputState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES gpuOutputState{D3D12_RESOURCE_STATE_COMMON};

    uint64_t fenceValue{0};
    uint64_t currentTaskId{0};
    uint64_t uploadUs{0};
    std::chrono::steady_clock::time_point submitTime;
    std::chrono::steady_clock::time_point uploadStartTime;

    bool ensureBuffers(ID3D12Device5* device, uint32_t w, uint32_t h) {
        if (valid && width == w && height == h) return true;

        const uint64_t sz = static_cast<uint64_t>(w) * h * 4;
        upload    = {};
        gpuInput  = {};
        gpuOutput = {};
        readback  = {};

        if (!upload.create(device, sz, d3d12::BufferUsage::Upload))   return false;
        if (!gpuInput.create(device, sz, d3d12::BufferUsage::Default)) return false;
        if (!gpuOutput.create(device, sz, d3d12::BufferUsage::Default))return false;
        if (!readback.create(device, sz, d3d12::BufferUsage::Readback))return false;

        width  = w;
        height = h;
        valid  = true;
        gpuInputState  = D3D12_RESOURCE_STATE_COMMON;
        gpuOutputState = D3D12_RESOURCE_STATE_COMMON;
        return true;
    }
};

// ─── PipelinedGpuExecutor::Impl ──────────────────────────────────────────────

struct PipelinedGpuExecutor::Impl {
    d3d12::D3D12Context                                   ctx;
    std::unordered_map<uint32_t, d3d12::ComputePipeline> pipelines;
    std::vector<PipelineSlot>                             slots;
    uint32_t                                              slotCount{2};

    ComPtr<ID3D12Fence>                                   fence;
    HANDLE                                                fenceEvent{nullptr};
    uint64_t                                              nextFenceValue{0};
    std::mutex                                            mu;
    uint32_t                                              frameCounter{0};

    ~Impl() {
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
    }
};

// ─── PipelinedGpuExecutor Implementation ────────────────────────────────────

PipelinedGpuExecutor::PipelinedGpuExecutor()
    : m_impl(std::make_unique<Impl>()) {}

PipelinedGpuExecutor::~PipelinedGpuExecutor() = default;

bool PipelinedGpuExecutor::init(const std::string& preferredAdapter, uint32_t slotCount) {
    if (slotCount == 0) slotCount = 2;
    m_impl->slotCount = slotCount;

    if (!m_impl->ctx.init(preferredAdapter)) return false;

    auto* device = m_impl->ctx.device();

    // Compile compute pipelines
    d3d12::ComputePipeline blurPso, edgePso;
    if (!blurPso.compileFromSource(device, kBlurHlsl)) {
        std::cerr << "[PipelinedGpuExecutor] Failed to compile blur shader\n";
        return false;
    }
    if (!edgePso.compileFromSource(device, kEdgeHlsl)) {
        std::cerr << "[PipelinedGpuExecutor] Failed to compile edge shader\n";
        return false;
    }

    m_impl->pipelines[static_cast<uint32_t>(protocol::TaskType::Blur)]
        = std::move(blurPso);
    m_impl->pipelines[static_cast<uint32_t>(protocol::TaskType::EdgeDetection)]
        = std::move(edgePso);

    // Initialise timeline fence and completion event
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_impl->fence)))) {
        std::cerr << "[PipelinedGpuExecutor] Failed to create timeline fence\n";
        return false;
    }
    m_impl->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_impl->fenceEvent) return false;

    // Allocate dedicated command allocator & command list per slot
    m_impl->slots.resize(slotCount);
    for (uint32_t i = 0; i < slotCount; ++i) {
        auto& slot = m_impl->slots[i];
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.cmdAlloc)))) {
            std::cerr << "[PipelinedGpuExecutor] Failed to create command allocator for slot " << i << '\n';
            return false;
        }
        if (FAILED(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.cmdAlloc.Get(), nullptr,
                IID_PPV_ARGS(&slot.cmdList)))) {
            std::cerr << "[PipelinedGpuExecutor] Failed to create command list for slot " << i << '\n';
            return false;
        }
        slot.cmdList->Close(); // Initially closed
    }

    return true;
}

bool PipelinedGpuExecutor::submitAsync(const protocol::TaskRequest& req, uint32_t slotIndex) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (slotIndex >= m_impl->slots.size()) return false;
    auto& slot = m_impl->slots[slotIndex];

    auto it = m_impl->pipelines.find(static_cast<uint32_t>(req.type));
    if (it == m_impl->pipelines.end()) return false;

    slot.uploadStartTime = std::chrono::steady_clock::now();
    if (!slot.ensureBuffers(m_impl->ctx.device(), req.width, req.height)) return false;
    if (!slot.upload.upload(req.input)) return false;

    auto uploadEndTime = std::chrono::steady_clock::now();
    slot.uploadUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(uploadEndTime - slot.uploadStartTime).count());

    // Reset command allocator and command list for this slot
    if (FAILED(slot.cmdAlloc->Reset())) return false;
    if (FAILED(slot.cmdList->Reset(slot.cmdAlloc.Get(), nullptr))) return false;

    auto* cl = slot.cmdList.Get();

    // 1. Copy from upload heap to default GPU input buffer
    transitionResource(cl, slot.gpuInput.resource(),
                       slot.gpuInputState,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(slot.gpuInput.resource(), slot.upload.resource());

    // 2. Transition resources for compute dispatch
    transitionResource(cl, slot.gpuInput.resource(),
                       slot.gpuInputState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionResource(cl, slot.gpuOutput.resource(),
                       slot.gpuOutputState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // 3. Bind compute pipeline and root descriptors
    auto& pso = it->second;
    pso.bind(cl);
    pso.setRootUAV(cl, slot.gpuOutput.resource()->GetGPUVirtualAddress(), 0);
    pso.setRootSRV(cl, slot.gpuInput.resource()->GetGPUVirtualAddress(), 1);

    static uint32_t s_radius = 0;
    static uint32_t s_iterations = 0;
    if (s_radius == 0) {
        char envVal[32] = {0};
        if (GetEnvironmentVariableA("DGPU_INTENSITY", envVal, sizeof(envVal)) > 0) {
            s_radius = static_cast<uint32_t>(std::atoi(envVal));
        }
        if (s_radius == 0) s_radius = 9;
    }
    if (s_iterations == 0) {
        char envVal[32] = {0};
        if (GetEnvironmentVariableA("DGPU_ITERATIONS", envVal, sizeof(envVal)) > 0) {
            s_iterations = static_cast<uint32_t>(std::atoi(envVal));
        }
        if (s_iterations == 0) s_iterations = 14;
    }
    uint32_t packedConfig = (s_radius & 0xFFu) | ((s_iterations & 0xFFu) << 8u);

    ++m_impl->frameCounter;
    uint32_t constants[4] = {req.width, req.height, packedConfig, m_impl->frameCounter};
    pso.setRootConstants(cl, constants, 4, 2);

    // 4. Dispatch compute kernel
    const uint32_t gx = (req.width  + 15) / 16;
    const uint32_t gy = (req.height + 15) / 16;
    cl->Dispatch(gx, gy, 1);

    // 5. Transition output to COPY_SOURCE and copy to readback buffer
    transitionResource(cl, slot.gpuOutput.resource(),
                       slot.gpuOutputState,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(slot.readback.resource(), slot.gpuOutput.resource());

    // 6. Restore GPU resources to COMMON
    transitionResource(cl, slot.gpuInput.resource(),
                       slot.gpuInputState,
                       D3D12_RESOURCE_STATE_COMMON);
    transitionResource(cl, slot.gpuOutput.resource(),
                       slot.gpuOutputState,
                       D3D12_RESOURCE_STATE_COMMON);

    if (FAILED(slot.cmdList->Close())) return false;

    // Execute asynchronously on the direct command queue
    ID3D12CommandList* lists[] = { slot.cmdList.Get() };
    m_impl->ctx.cmdQueue()->ExecuteCommandLists(1, lists);

    // Signal timeline fence
    ++m_impl->nextFenceValue;
    if (FAILED(m_impl->ctx.cmdQueue()->Signal(m_impl->fence.Get(), m_impl->nextFenceValue))) {
        return false;
    }

    slot.fenceValue    = m_impl->nextFenceValue;
    slot.inFlight      = true;
    slot.currentTaskId = req.taskId;
    slot.submitTime    = std::chrono::steady_clock::now();

    return true;
}

protocol::TaskResult PipelinedGpuExecutor::collect(uint32_t slotIndex) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    protocol::TaskResult res;
    if (slotIndex >= m_impl->slots.size()) {
        res.status = protocol::TaskStatus::InternalError;
        return res;
    }

    auto& slot = m_impl->slots[slotIndex];
    if (!slot.inFlight) {
        res.status = protocol::TaskStatus::InternalError;
        return res;
    }

    res.taskId = slot.currentTaskId;

    // Block until GPU reaches the fence value for this slot
    if (m_impl->fence->GetCompletedValue() < slot.fenceValue) {
        if (FAILED(m_impl->fence->SetEventOnCompletion(slot.fenceValue, m_impl->fenceEvent))) {
            res.status = protocol::TaskStatus::GpuError;
            return res;
        }
        WaitForSingleObject(m_impl->fenceEvent, INFINITE);
    }

    auto gpuDoneTime = std::chrono::steady_clock::now();
    res.gpuUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(gpuDoneTime - slot.submitTime).count());

    // Read back results into host CPU memory
    auto rbStart = std::chrono::steady_clock::now();
    if (!slot.readback.readback(res.output)) {
        res.status = protocol::TaskStatus::GpuError;
        return res;
    }
    auto rbEnd = std::chrono::steady_clock::now();
    res.readbackUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(rbEnd - rbStart).count());

    res.uploadUs = slot.uploadUs;
    res.totalUs  = res.uploadUs + res.gpuUs + res.readbackUs;
    res.status   = protocol::TaskStatus::Ok;

    slot.inFlight = false;
    return res;
}

protocol::TaskResult PipelinedGpuExecutor::execute(const protocol::TaskRequest& req) {
    if (!submitAsync(req, 0)) {
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::GpuError;
        return err;
    }
    return collect(0);
}

std::vector<protocol::TaskResult> PipelinedGpuExecutor::executePipelined(
    const std::vector<protocol::TaskRequest>& tasks,
    PipelinedExecutionMetrics* outMetrics) {

    if (tasks.empty()) return {};

    const size_t N = tasks.size();
    std::vector<protocol::TaskResult> results(N);

    auto wallClockStart = std::chrono::steady_clock::now();
    uint64_t sumGpu = 0, sumUp = 0, sumRb = 0;

    for (size_t i = 0; i < N; ++i) {
        uint32_t slot = static_cast<uint32_t>(i % m_impl->slotCount);

        // If this slot currently has work in flight, collect it before re-submitting
        if (m_impl->slots[slot].inFlight) {
            size_t prevTaskIdx = i - m_impl->slotCount;
            results[prevTaskIdx] = collect(slot);
            sumGpu += results[prevTaskIdx].gpuUs;
            sumUp  += results[prevTaskIdx].uploadUs;
            sumRb  += results[prevTaskIdx].readbackUs;
        }

        if (!submitAsync(tasks[i], slot)) {
            results[i].taskId = tasks[i].taskId;
            results[i].status = protocol::TaskStatus::GpuError;
        }
    }

    // Collect any remaining in-flight slots
    const size_t tailStart = (N >= m_impl->slotCount) ? (N - m_impl->slotCount) : 0;
    for (size_t i = tailStart; i < N; ++i) {
        uint32_t slot = static_cast<uint32_t>(i % m_impl->slotCount);
        if (m_impl->slots[slot].inFlight) {
            results[i] = collect(slot);
            sumGpu += results[i].gpuUs;
            sumUp  += results[i].uploadUs;
            sumRb  += results[i].readbackUs;
        }
    }

    auto wallClockEnd = std::chrono::steady_clock::now();
    uint64_t totalWallUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(wallClockEnd - wallClockStart).count());

    if (outMetrics) {
        outMetrics->totalTasks            = static_cast<uint32_t>(N);
        outMetrics->slotCount             = m_impl->slotCount;
        outMetrics->totalWallClockUs      = totalWallUs;
        outMetrics->sumGpuComputeUs       = sumGpu;
        outMetrics->sumUploadUs           = sumUp;
        outMetrics->sumReadbackUs         = sumRb;
        outMetrics->throughputTasksPerSec = (totalWallUs > 0)
            ? (static_cast<double>(N) * 1'000'000.0 / static_cast<double>(totalWallUs))
            : 0.0;
        uint64_t sumStages = sumGpu + sumUp + sumRb;
        outMetrics->speedupVsSequential = (totalWallUs > 0 && sumStages > 0)
            ? (static_cast<double>(sumStages) / static_cast<double>(totalWallUs))
            : 1.0;
        outMetrics->overlapRatio = (sumStages > totalWallUs && sumStages > 0)
            ? static_cast<double>(sumStages - totalWallUs) / static_cast<double>(sumStages)
            : 0.0;
    }

    return results;
}

uint32_t PipelinedGpuExecutor::slotCount() const noexcept {
    return m_impl ? m_impl->slotCount : 0;
}

const d3d12::AdapterInfo& PipelinedGpuExecutor::adapterInfo() const noexcept {
    return m_impl->ctx.adapterInfo();
}

} // namespace dgpu::gpu_executor
