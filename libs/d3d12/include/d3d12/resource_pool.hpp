// d3d12/resource_pool.hpp
// High-performance fence-synchronized D3D12 buffer and command allocator pool (Phase 2.4).
// Eliminates runtime committed resource allocations and heap fragmentation during steady-state compute.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "d3d12/d3d12_context.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dgpu::d3d12 {

// ─── Pool Performance Metrics ────────────────────────────────────────────────

struct ResourcePoolMetrics {
    uint64_t poolHits{0};
    uint64_t poolMisses{0};
    uint64_t totalAllocations{0};
    uint64_t activeAllocations{0};
    uint64_t totalAllocatedBytes{0};
    uint64_t activeVramBytes{0};
    uint64_t maxVramBytesObserved{0};
    double   hitRatio{0.0};
};

// ─── Pooled GpuBuffer Handle ─────────────────────────────────────────────────

struct PooledGpuBuffer {
    GpuBuffer   buffer;
    uint64_t    sizeBytes{0};
    BufferUsage usage{BufferUsage::Default};
    uint64_t    lastFenceValue{0};
    bool        inUse{false};
    uint32_t    id{0};
};

// ─── D3D12ResourcePool ───────────────────────────────────────────────────────
// Thread-safe, fence-synchronized pooling layer for:
// 1. Upload, Default, and Readback GPU buffers.
// 2. Direct Command Allocators.
// 3. VRAM bounds tracking and budget enforcement for 4GB VRAM target GPUs.

class D3D12ResourcePool {
public:
    explicit D3D12ResourcePool(uint64_t maxVramBudgetBytes = 3ull * 1024ull * 1024ull * 1024ull);
    ~D3D12ResourcePool();

    D3D12ResourcePool(const D3D12ResourcePool&)            = delete;
    D3D12ResourcePool& operator=(const D3D12ResourcePool&) = delete;

    // Acquires a buffer from the pool if an idle buffer with matching usage and
    // sufficient capacity exists whose GPU work has completed (fence completed).
    // Otherwise allocates a new committed resource up to the VRAM budget limit.
    [[nodiscard]] PooledGpuBuffer* acquireBuffer(
        ID3D12Device5* device,
        uint64_t       sizeBytes,
        BufferUsage    usage,
        uint64_t       currentCompletedFence);

    // Returns a buffer to the pool, marking it with the fence value when its GPU work will finish
    void releaseBuffer(PooledGpuBuffer* buffer, uint64_t completionFenceValue);

    // Command allocator pooling
    [[nodiscard]] ID3D12CommandAllocator* acquireCommandAllocator(
        ID3D12Device5* device,
        D3D12_COMMAND_LIST_TYPE type,
        uint64_t currentCompletedFence);

    void releaseCommandAllocator(
        ID3D12CommandAllocator* alloc,
        uint64_t completionFenceValue);

    // Frees pooled buffers that are not in use and whose GPU work has completed
    void trim(uint64_t currentCompletedFence);

    // Metrics and active state
    [[nodiscard]] ResourcePoolMetrics metrics() const noexcept;
    [[nodiscard]] uint64_t activeVramBytes() const noexcept;
    [[nodiscard]] uint64_t vramBudgetBytes() const noexcept;
    void resetMetrics() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::d3d12
