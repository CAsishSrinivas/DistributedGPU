// d3d12/d3d12_context.hpp
// Device initialisation, command infrastructure, and resource helpers (M1 / M3).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Forward-declare D3D12 types so that callers that only need the context
// interface don't have to include the full DirectX headers.
struct ID3D12Device5;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList4;
struct ID3D12Fence;
struct IDXGIAdapter1;

namespace dgpu::d3d12 {

// ─── Adapter info (returned by enumeration) ───────────────────────────────────

struct AdapterInfo {
    std::string name;
    uint64_t    dedicatedVideoMemoryBytes{};
    bool        isSoftware{false};
    uint32_t    index{};          // DXGI adapter index
};

[[nodiscard]] std::vector<AdapterInfo> enumerateAdapters();

// ─── D3D12Context ────────────────────────────────────────────────────────────
// Owns the device, one direct command queue, one command allocator,
// one command list and a fence for CPU/GPU synchronisation.
// One context is sufficient for the single-producer compute pipeline.

class D3D12Context {
public:
    D3D12Context();
    ~D3D12Context();

    D3D12Context(const D3D12Context&)            = delete;
    D3D12Context& operator=(const D3D12Context&) = delete;
    D3D12Context(D3D12Context&&)                 noexcept;
    D3D12Context& operator=(D3D12Context&&)      noexcept;

    // Initialise on a specific adapter (prefer by name fragment, e.g. "RTX 3050")
    [[nodiscard]] bool init(const std::string& preferredAdapterSubstring = "");

    // Flush and block until the GPU is idle
    void waitIdle();

    // Accessors (raw pointers; lifetime is managed by this object)
    [[nodiscard]] ID3D12Device5*               device()      const noexcept;
    [[nodiscard]] ID3D12CommandQueue*           cmdQueue()    const noexcept;
    [[nodiscard]] ID3D12CommandAllocator*       cmdAlloc()    const noexcept;
    [[nodiscard]] ID3D12GraphicsCommandList4*   cmdList()     const noexcept;

    [[nodiscard]] const AdapterInfo& adapterInfo() const noexcept;

    // Reset the command allocator and command list, ready to record new work
    [[nodiscard]] bool resetCommandList();

    // Execute the command list and wait for completion (synchronous helper)
    void executeAndWait();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ─── GpuBuffer ───────────────────────────────────────────────────────────────
// Wraps a committed resource with optional upload and readback staging buffers.

enum class BufferUsage { Default, Upload, Readback };

class GpuBuffer {
public:
    GpuBuffer();
    ~GpuBuffer();

    GpuBuffer(const GpuBuffer&)            = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&&)                 noexcept;
    GpuBuffer& operator=(GpuBuffer&&)      noexcept;

    [[nodiscard]] bool create(ID3D12Device5* device,
                              uint64_t       sizeBytes,
                              BufferUsage    usage);

    // Upload CPU data into an upload-heap buffer
    [[nodiscard]] bool upload(std::span<const std::byte> data);

    // Read back GPU data into a CPU vector (call after GPU work completes)
    [[nodiscard]] bool readback(std::vector<std::byte>& out);

    [[nodiscard]] uint64_t     sizeBytes() const noexcept;
    [[nodiscard]] BufferUsage  usage()     const noexcept;

    // Raw resource pointer (for binding to root signatures / UAVs)
    struct ID3D12Resource* resource() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ─── ComputePipeline ─────────────────────────────────────────────────────────
// Compiles an HLSL compute shader (or loads a pre-compiled CSO) and creates
// the root signature + PSO pair needed to dispatch it.

class ComputePipeline {
public:
    ComputePipeline();
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&)            = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&&)                 noexcept;
    ComputePipeline& operator=(ComputePipeline&&)      noexcept;

    // Compile from HLSL source string (uses D3DCompile at runtime)
    [[nodiscard]] bool compileFromSource(ID3D12Device5*   device,
                                          const std::string& hlslSource,
                                          const std::string& entryPoint = "CSMain",
                                          const std::string& target     = "cs_5_1");

    // Load a pre-compiled CSO blob (preferred for shipping builds)
    [[nodiscard]] bool loadFromCso(ID3D12Device5*              device,
                                    std::span<const std::byte> csoBytes);

    void bind(ID3D12GraphicsCommandList4* cmdList) const;

    // Root parameter slot 0: UAV (output), slot 1: SRV (input)
    // slot 2: root constants (width, height, params)
    void setRootUAV(ID3D12GraphicsCommandList4* cmdList,
                    uint64_t gpuVa, uint32_t slot = 0) const;
    void setRootSRV(ID3D12GraphicsCommandList4* cmdList,
                    uint64_t gpuVa, uint32_t slot = 1) const;
    void setRootConstants(ID3D12GraphicsCommandList4* cmdList,
                           const void* data, uint32_t numDwords,
                           uint32_t slot = 2) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ─── GpuTimer ────────────────────────────────────────────────────────────────
// Records GPU timestamps around a dispatch and resolves them after the fence.

class GpuTimer {
public:
    GpuTimer();
    ~GpuTimer();

    GpuTimer(const GpuTimer&)            = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    [[nodiscard]] bool init(ID3D12Device5* device, ID3D12CommandQueue* queue);

    void begin(ID3D12GraphicsCommandList4* cmdList);
    void end(ID3D12GraphicsCommandList4* cmdList);

    // Call after the fence has been signalled (GPU work is done)
    [[nodiscard]] uint64_t elapsedMicroseconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::d3d12
