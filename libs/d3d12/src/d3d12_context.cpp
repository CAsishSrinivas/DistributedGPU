// d3d12/d3d12_context.cpp
// DirectX 12 device initialisation, buffer management, compute pipeline, and timers.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include "d3d12/d3d12_context.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

// ─── Helper macros ────────────────────────────────────────────────────────────

#define D3D_CHECK(hr, msg)                                                  \
    do {                                                                     \
        if (FAILED(hr)) {                                                    \
            std::cerr << "[d3d12] " << (msg) << " HRESULT=0x"               \
                      << std::hex << (hr) << std::dec << '\n';               \
            return false;                                                     \
        }                                                                     \
    } while (0)

namespace dgpu::d3d12 {

// ─── Adapter enumeration ─────────────────────────────────────────────────────

std::vector<AdapterInfo> enumerateAdapters() {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};

    std::vector<AdapterInfo> result;
    ComPtr<IDXGIAdapter1> adapter;
    for (uint32_t i = 0;
         factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) == S_OK;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        AdapterInfo info;
        // Convert WCHAR to UTF-8 for logging
        char buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            buf, sizeof(buf), nullptr, nullptr);
        info.name                      = buf;
        info.dedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;
        info.isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        info.index      = i;
        result.push_back(info);
        adapter.Reset();
    }
    return result;
}

// ─── D3D12Context::Impl ──────────────────────────────────────────────────────

struct D3D12Context::Impl {
    ComPtr<ID3D12Device5>              device;
    ComPtr<ID3D12CommandQueue>         cmdQueue;
    ComPtr<ID3D12CommandAllocator>     cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList4> cmdList;
    ComPtr<ID3D12Fence>                fence;
    HANDLE                             fenceEvent{nullptr};
    uint64_t                           fenceValue{0};
    AdapterInfo                        adapterInfo;

    bool signalAndWait() {
        ++fenceValue;
        if (FAILED(cmdQueue->Signal(fence.Get(), fenceValue))) return false;
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
        return true;
    }
};

// ─── D3D12Context ────────────────────────────────────────────────────────────

D3D12Context::D3D12Context()  : m_impl(std::make_unique<Impl>()) {}
D3D12Context::~D3D12Context() {
    if (m_impl) {
        waitIdle();
        if (m_impl->fenceEvent) CloseHandle(m_impl->fenceEvent);
    }
}
D3D12Context::D3D12Context(D3D12Context&&) noexcept = default;
D3D12Context& D3D12Context::operator=(D3D12Context&&) noexcept = default;

bool D3D12Context::init(const std::string& preferredAdapter) {
#ifdef DGPU_DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        std::cout << "[d3d12] Debug layer enabled\n";
    }
#endif

    ComPtr<IDXGIFactory6> factory;
    D3D_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    // Find best matching adapter
    ComPtr<IDXGIAdapter1> selectedAdapter;
    auto adapters = enumerateAdapters();
    for (const auto& info : adapters) {
        if (info.isSoftware) continue;
        if (!preferredAdapter.empty()) {
            if (info.name.find(preferredAdapter) == std::string::npos) continue;
        }
        factory->EnumAdapterByGpuPreference(
            info.index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&selectedAdapter));
        m_impl->adapterInfo = info;
        break;
    }

    // Fall back to first hardware adapter
    if (!selectedAdapter) {
        for (const auto& info : adapters) {
            if (info.isSoftware) continue;
            factory->EnumAdapterByGpuPreference(
                info.index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&selectedAdapter));
            m_impl->adapterInfo = info;
            break;
        }
    }

    if (!selectedAdapter) {
        std::cerr << "[d3d12] No hardware adapter found\n";
        return false;
    }

    std::cout << "[d3d12] Using adapter: " << m_impl->adapterInfo.name
              << " (" << m_impl->adapterInfo.dedicatedVideoMemoryBytes / (1024 * 1024)
              << " MB VRAM)\n";

    D3D_CHECK(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                IID_PPV_ARGS(&m_impl->device)),
              "D3D12CreateDevice");

    // Command queue
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    D3D_CHECK(m_impl->device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_impl->cmdQueue)),
              "CreateCommandQueue");

    // Command allocator + list
    D3D_CHECK(m_impl->device->CreateCommandAllocator(
                  D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_impl->cmdAlloc)),
              "CreateCommandAllocator");

    D3D_CHECK(m_impl->device->CreateCommandList(
                  0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                  m_impl->cmdAlloc.Get(), nullptr,
                  IID_PPV_ARGS(&m_impl->cmdList)),
              "CreateCommandList");
    m_impl->cmdList->Close(); // Start in closed state; caller calls resetCommandList()

    // Fence
    D3D_CHECK(m_impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&m_impl->fence)),
              "CreateFence");
    m_impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_impl->fenceEvent) {
        std::cerr << "[d3d12] CreateEvent failed\n";
        return false;
    }

    return true;
}

void D3D12Context::waitIdle() {
    if (m_impl && m_impl->cmdQueue) m_impl->signalAndWait();
}

ID3D12Device5* D3D12Context::device() const noexcept {
    return m_impl ? m_impl->device.Get() : nullptr;
}
ID3D12CommandQueue* D3D12Context::cmdQueue() const noexcept {
    return m_impl ? m_impl->cmdQueue.Get() : nullptr;
}
ID3D12CommandAllocator* D3D12Context::cmdAlloc() const noexcept {
    return m_impl ? m_impl->cmdAlloc.Get() : nullptr;
}
ID3D12GraphicsCommandList4* D3D12Context::cmdList() const noexcept {
    return m_impl ? m_impl->cmdList.Get() : nullptr;
}
const AdapterInfo& D3D12Context::adapterInfo() const noexcept {
    static AdapterInfo empty{};
    return m_impl ? m_impl->adapterInfo : empty;
}

bool D3D12Context::resetCommandList() {
    if (FAILED(m_impl->cmdAlloc->Reset())) return false;
    if (FAILED(m_impl->cmdList->Reset(m_impl->cmdAlloc.Get(), nullptr))) return false;
    return true;
}

void D3D12Context::executeAndWait() {
    m_impl->cmdList->Close();
    ID3D12CommandList* lists[] = {m_impl->cmdList.Get()};
    m_impl->cmdQueue->ExecuteCommandLists(1, lists);
    m_impl->signalAndWait();
}

// ─── GpuBuffer::Impl ─────────────────────────────────────────────────────────

struct GpuBuffer::Impl {
    ComPtr<ID3D12Resource> resource;
    uint64_t               sizeBytes{};
    BufferUsage            usage{BufferUsage::Default};
};

GpuBuffer::GpuBuffer()  : m_impl(std::make_unique<Impl>()) {}
GpuBuffer::~GpuBuffer() = default;
GpuBuffer::GpuBuffer(GpuBuffer&&) noexcept = default;
GpuBuffer& GpuBuffer::operator=(GpuBuffer&&) noexcept = default;

bool GpuBuffer::create(ID3D12Device5* device, uint64_t sizeBytes, BufferUsage usage) {
    m_impl->sizeBytes = sizeBytes;
    m_impl->usage     = usage;

    D3D12_HEAP_PROPERTIES heap{};
    D3D12_RESOURCE_STATES initState{};

    switch (usage) {
    case BufferUsage::Default:
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        initState = D3D12_RESOURCE_STATE_COMMON;
        break;
    case BufferUsage::Upload:
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        initState = D3D12_RESOURCE_STATE_GENERIC_READ;
        break;
    case BufferUsage::Readback:
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        initState = D3D12_RESOURCE_STATE_COPY_DEST;
        break;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width              = sizeBytes;
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (usage == BufferUsage::Default) {
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initState,
        nullptr, IID_PPV_ARGS(&m_impl->resource));
    if (FAILED(hr)) {
        std::cerr << "[GpuBuffer] CreateCommittedResource failed 0x"
                  << std::hex << hr << std::dec;
        if (hr == DXGI_ERROR_DEVICE_REMOVED && device) {
            std::cerr << " (Device Removed: 0x" << std::hex << device->GetDeviceRemovedReason() << std::dec << ")";
        }
        std::cerr << '\n';
        return false;
    }
    return true;
}

bool GpuBuffer::upload(std::span<const std::byte> data) {
    assert(m_impl->usage == BufferUsage::Upload);
    void* mapped = nullptr;
    if (FAILED(m_impl->resource->Map(0, nullptr, &mapped))) return false;
    std::memcpy(mapped, data.data(), data.size());
    m_impl->resource->Unmap(0, nullptr);
    return true;
}

bool GpuBuffer::readback(std::vector<std::byte>& out) {
    assert(m_impl->usage == BufferUsage::Readback);
    out.resize(m_impl->sizeBytes);
    void* mapped = nullptr;
    D3D12_RANGE range{0, m_impl->sizeBytes};
    if (FAILED(m_impl->resource->Map(0, &range, &mapped))) return false;
    std::memcpy(out.data(), mapped, m_impl->sizeBytes);
    D3D12_RANGE empty{0, 0};
    m_impl->resource->Unmap(0, &empty);
    return true;
}

uint64_t    GpuBuffer::sizeBytes() const noexcept { return m_impl->sizeBytes; }
BufferUsage GpuBuffer::usage()     const noexcept { return m_impl->usage;     }
ID3D12Resource* GpuBuffer::resource() const noexcept {
    return m_impl ? m_impl->resource.Get() : nullptr;
}

// ─── ComputePipeline::Impl ───────────────────────────────────────────────────

struct ComputePipeline::Impl {
    ComPtr<ID3D12RootSignature>  rootSig;
    ComPtr<ID3D12PipelineState>  pso;
};

ComputePipeline::ComputePipeline()  : m_impl(std::make_unique<Impl>()) {}
ComputePipeline::~ComputePipeline() = default;
ComputePipeline::ComputePipeline(ComputePipeline&&) noexcept = default;
ComputePipeline& ComputePipeline::operator=(ComputePipeline&&) noexcept = default;

static bool createRootSignature(ID3D12Device5* device,
                                 ComPtr<ID3D12RootSignature>& out) {
    // Slot 0: UAV (output buffer)   — D3D12_ROOT_PARAMETER_TYPE_UAV
    // Slot 1: SRV (input buffer)    — D3D12_ROOT_PARAMETER_TYPE_SRV
    // Slot 2: Root constants        — 4 DWORDs (width, height, param0, param1)
    D3D12_ROOT_PARAMETER params[3]{};

    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace  = 0;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.RegisterSpace  = 0;
    params[2].Constants.Num32BitValues = 4;
    params[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = 3;
    rsDesc.pParameters       = params;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                           &blob, &error))) {
        if (error) std::cerr << (char*)error->GetBufferPointer() << '\n';
        return false;
    }
    HRESULT hr = device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&out));
    return SUCCEEDED(hr);
}

bool ComputePipeline::compileFromSource(ID3D12Device5*     device,
                                         const std::string& hlsl,
                                         const std::string& entry,
                                         const std::string& target) {
    if (!createRootSignature(device, m_impl->rootSig)) return false;

    ComPtr<ID3DBlob> shader, error;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef DGPU_DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(hlsl.c_str(), hlsl.size(), nullptr, nullptr, nullptr,
                             entry.c_str(), target.c_str(), flags, 0,
                             &shader, &error);
    if (FAILED(hr)) {
        if (error) std::cerr << "[ComputePipeline] " << (char*)error->GetBufferPointer();
        return false;
    }
    return loadFromCso(device, {reinterpret_cast<const std::byte*>(shader->GetBufferPointer()),
                                  shader->GetBufferSize()});
}

bool ComputePipeline::loadFromCso(ID3D12Device5*              device,
                                   std::span<const std::byte>  cso) {
    if (!m_impl->rootSig && !createRootSignature(device, m_impl->rootSig)) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature     = m_impl->rootSig.Get();
    psoDesc.CS.pShaderBytecode = cso.data();
    psoDesc.CS.BytecodeLength  = cso.size();

    HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_impl->pso));
    return SUCCEEDED(hr);
}

void ComputePipeline::bind(ID3D12GraphicsCommandList4* cmdList) const {
    cmdList->SetComputeRootSignature(m_impl->rootSig.Get());
    cmdList->SetPipelineState(m_impl->pso.Get());
}

void ComputePipeline::setRootUAV(ID3D12GraphicsCommandList4* cl, uint64_t va, uint32_t slot) const {
    cl->SetComputeRootUnorderedAccessView(slot, va);
}
void ComputePipeline::setRootSRV(ID3D12GraphicsCommandList4* cl, uint64_t va, uint32_t slot) const {
    cl->SetComputeRootShaderResourceView(slot, va);
}
void ComputePipeline::setRootConstants(ID3D12GraphicsCommandList4* cl,
                                        const void* data, uint32_t numDwords,
                                        uint32_t slot) const {
    cl->SetComputeRoot32BitConstants(slot, numDwords, data, 0);
}

// ─── GpuTimer::Impl ──────────────────────────────────────────────────────────

struct GpuTimer::Impl {
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource>  readbackBuf;
    uint64_t                freq{0};      // ticks per second
    mutable uint64_t        elapsedUs{0};
};

GpuTimer::GpuTimer()  : m_impl(std::make_unique<Impl>()) {}
GpuTimer::~GpuTimer() = default;

bool GpuTimer::init(ID3D12Device5* device, ID3D12CommandQueue* queue) {
    if (FAILED(queue->GetTimestampFrequency(&m_impl->freq))) return false;

    D3D12_QUERY_HEAP_DESC hDesc{};
    hDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hDesc.Count = 2; // begin + end
    if (FAILED(device->CreateQueryHeap(&hDesc, IID_PPV_ARGS(&m_impl->queryHeap))))
        return false;

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC   rDesc{};
    rDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rDesc.Width              = sizeof(uint64_t) * 2;
    rDesc.Height             = 1;
    rDesc.DepthOrArraySize   = 1;
    rDesc.MipLevels          = 1;
    rDesc.SampleDesc.Count   = 1;
    rDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &rDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_impl->readbackBuf)));
}

void GpuTimer::begin(ID3D12GraphicsCommandList4* cl) {
    cl->EndQuery(m_impl->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
}

void GpuTimer::end(ID3D12GraphicsCommandList4* cl) {
    cl->EndQuery(m_impl->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    cl->ResolveQueryData(m_impl->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                         0, 2, m_impl->readbackBuf.Get(), 0);
}

uint64_t GpuTimer::elapsedMicroseconds() const {
    void* mapped = nullptr;
    D3D12_RANGE range{0, sizeof(uint64_t) * 2};
    if (FAILED(m_impl->readbackBuf->Map(0, &range, &mapped))) return 0;
    uint64_t ts[2]{};
    std::memcpy(ts, mapped, sizeof(ts));
    D3D12_RANGE empty{0, 0};
    m_impl->readbackBuf->Unmap(0, &empty);

    uint64_t deltaTicks = ts[1] - ts[0];
    return (deltaTicks * 1'000'000ULL) / m_impl->freq;
}

} // namespace dgpu::d3d12
