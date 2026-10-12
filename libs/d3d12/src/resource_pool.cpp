// libs/d3d12/src/resource_pool.cpp
// High-performance fence-synchronized D3D12 buffer and command allocator pool (Phase 2.4).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "d3d12/resource_pool.hpp"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dgpu::d3d12 {

// ─── Internal Structures ─────────────────────────────────────────────────────

struct PooledCommandAllocator {
    ComPtr<ID3D12CommandAllocator> alloc;
    D3D12_COMMAND_LIST_TYPE        type{D3D12_COMMAND_LIST_TYPE_DIRECT};
    uint64_t                       lastFenceValue{0};
    bool                           inUse{false};
};

struct D3D12ResourcePool::Impl {
    uint64_t                                            vramBudgetBytes;
    uint64_t                                            activeVramBytes{0};
    ResourcePoolMetrics                                 metrics{};
    std::vector<std::unique_ptr<PooledGpuBuffer>>       buffers;
    std::vector<PooledCommandAllocator>                 cmdAllocators;
    mutable std::mutex                                  mu;

    explicit Impl(uint64_t budget) : vramBudgetBytes(budget) {}

    void trimUnlocked(uint64_t currentCompletedFence) {
        auto it = std::remove_if(buffers.begin(), buffers.end(),
            [this, currentCompletedFence](const std::unique_ptr<PooledGpuBuffer>& b) {
                if (!b->inUse && b->lastFenceValue <= currentCompletedFence) {
                    if (activeVramBytes >= b->sizeBytes) {
                        activeVramBytes -= b->sizeBytes;
                    } else {
                        activeVramBytes = 0;
                    }
                    return true;
                }
                return false;
            });
        buffers.erase(it, buffers.end());
        metrics.activeVramBytes = activeVramBytes;
        metrics.activeAllocations = buffers.size();
    }
};

// ─── D3D12ResourcePool Implementation ────────────────────────────────────────

D3D12ResourcePool::D3D12ResourcePool(uint64_t maxVramBudgetBytes)
    : m_impl(std::make_unique<Impl>(maxVramBudgetBytes)) {}

D3D12ResourcePool::~D3D12ResourcePool() = default;

PooledGpuBuffer* D3D12ResourcePool::acquireBuffer(
    ID3D12Device5* device,
    uint64_t       sizeBytes,
    BufferUsage    usage,
    uint64_t       currentCompletedFence) {

    std::lock_guard<std::mutex> lk(m_impl->mu);

    // Align to 64 KB boundary to maximize bucket reuse across near sizes
    const uint64_t alignedSize = (sizeBytes + 65535ull) & ~65535ull;

    // Search for best-fit idle buffer
    PooledGpuBuffer* bestMatch = nullptr;
    uint64_t bestSizeDiff = UINT64_MAX;

    for (const auto& entry : m_impl->buffers) {
        if (!entry->inUse && entry->usage == usage &&
            entry->sizeBytes >= alignedSize &&
            entry->lastFenceValue <= currentCompletedFence) {

            uint64_t diff = entry->sizeBytes - alignedSize;
            if (diff < bestSizeDiff) {
                bestMatch = entry.get();
                bestSizeDiff = diff;
                if (diff == 0) break; // Exact match found
            }
        }
    }

    if (bestMatch) {
        bestMatch->inUse = true;
        ++m_impl->metrics.poolHits;
        uint64_t totalRequests = m_impl->metrics.poolHits + m_impl->metrics.poolMisses;
        m_impl->metrics.hitRatio = static_cast<double>(m_impl->metrics.poolHits) / static_cast<double>(totalRequests);
        return bestMatch;
    }

    // Check VRAM budget and trim completed resources if necessary
    if (m_impl->activeVramBytes + alignedSize > m_impl->vramBudgetBytes) {
        m_impl->trimUnlocked(currentCompletedFence);
    }

    // Allocate new committed buffer
    auto newBuf = std::make_unique<PooledGpuBuffer>();
    if (!newBuf->buffer.create(device, alignedSize, usage)) {
        std::cerr << "[D3D12ResourcePool] Failed to create committed buffer of "
                  << alignedSize << " bytes (Usage=" << static_cast<int>(usage) << ")\n";
        return nullptr;
    }

    newBuf->sizeBytes      = alignedSize;
    newBuf->usage          = usage;
    newBuf->inUse          = true;
    newBuf->lastFenceValue = 0;
    newBuf->id             = static_cast<uint32_t>(m_impl->buffers.size() + 1);

    m_impl->activeVramBytes += alignedSize;
    m_impl->metrics.totalAllocatedBytes += alignedSize;
    m_impl->metrics.activeVramBytes = m_impl->activeVramBytes;
    m_impl->metrics.maxVramBytesObserved = std::max(m_impl->metrics.maxVramBytesObserved, m_impl->activeVramBytes);
    ++m_impl->metrics.poolMisses;
    ++m_impl->metrics.totalAllocations;
    m_impl->metrics.activeAllocations = m_impl->buffers.size() + 1;

    uint64_t totalRequests = m_impl->metrics.poolHits + m_impl->metrics.poolMisses;
    m_impl->metrics.hitRatio = static_cast<double>(m_impl->metrics.poolHits) / static_cast<double>(totalRequests);

    auto* resultPtr = newBuf.get();
    m_impl->buffers.push_back(std::move(newBuf));
    return resultPtr;
}

void D3D12ResourcePool::releaseBuffer(PooledGpuBuffer* buffer, uint64_t completionFenceValue) {
    if (!buffer) return;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    buffer->lastFenceValue = completionFenceValue;
    buffer->inUse          = false;
}

ID3D12CommandAllocator* D3D12ResourcePool::acquireCommandAllocator(
    ID3D12Device5* device,
    D3D12_COMMAND_LIST_TYPE type,
    uint64_t currentCompletedFence) {

    std::lock_guard<std::mutex> lk(m_impl->mu);

    for (auto& entry : m_impl->cmdAllocators) {
        if (!entry.inUse && entry.type == type && entry.lastFenceValue <= currentCompletedFence) {
            if (SUCCEEDED(entry.alloc->Reset())) {
                entry.inUse = true;
                return entry.alloc.Get();
            }
        }
    }

    // Allocate new command allocator
    ComPtr<ID3D12CommandAllocator> newAlloc;
    if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&newAlloc)))) {
        std::cerr << "[D3D12ResourcePool] Failed to create command allocator\n";
        return nullptr;
    }

    PooledCommandAllocator entry;
    entry.alloc          = newAlloc;
    entry.type           = type;
    entry.lastFenceValue = 0;
    entry.inUse          = true;

    auto* ret = newAlloc.Get();
    m_impl->cmdAllocators.push_back(std::move(entry));
    return ret;
}

void D3D12ResourcePool::releaseCommandAllocator(
    ID3D12CommandAllocator* alloc,
    uint64_t completionFenceValue) {

    if (!alloc) return;
    std::lock_guard<std::mutex> lk(m_impl->mu);

    for (auto& entry : m_impl->cmdAllocators) {
        if (entry.alloc.Get() == alloc) {
            entry.lastFenceValue = completionFenceValue;
            entry.inUse          = false;
            break;
        }
    }
}

void D3D12ResourcePool::trim(uint64_t currentCompletedFence) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->trimUnlocked(currentCompletedFence);
}

ResourcePoolMetrics D3D12ResourcePool::metrics() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->metrics;
}

uint64_t D3D12ResourcePool::activeVramBytes() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->activeVramBytes;
}

uint64_t D3D12ResourcePool::vramBudgetBytes() const noexcept {
    return m_impl->vramBudgetBytes;
}

void D3D12ResourcePool::resetMetrics() noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->metrics.poolHits   = 0;
    m_impl->metrics.poolMisses = 0;
    m_impl->metrics.hitRatio   = 0.0;
}

} // namespace dgpu::d3d12
