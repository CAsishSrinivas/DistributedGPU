// apps/game_adapter/d3d12_game_hook.cpp
// Implementation of Milestone M7 game offload adapter and frame pacer.

#include "d3d12_game_hook.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>

namespace dgpu::game_adapter {

// ─── FramePacer Implementation ────────────────────────────────────────────────

void FramePacer::recordFrame(uint64_t frameTimeUs) {
    if (frameTimeUs > 0) {
        m_frameTimesUs.push_back(frameTimeUs);
    }
}

void FramePacer::reset() {
    m_frameTimesUs.clear();
}

FramePacingStats FramePacer::computeStats() const {
    FramePacingStats stats{};
    if (m_frameTimesUs.empty()) return stats;

    stats.totalFrames = m_frameTimesUs.size();

    // Copy and sort for percentiles
    std::vector<uint64_t> sorted = m_frameTimesUs;
    std::sort(sorted.begin(), sorted.end());

    double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    stats.meanFrameUs = sum / static_cast<double>(sorted.size());
    stats.avgFps      = 1'000'000.0 / stats.meanFrameUs;

    // 1% Low FPS (99th percentile highest frame time)
    size_t p99Idx = static_cast<size_t>(std::ceil(0.99 * sorted.size())) - 1;
    p99Idx = std::clamp(p99Idx, size_t{0}, sorted.size() - 1);
    stats.p99FrameUs    = static_cast<double>(sorted[p99Idx]);
    stats.onePercentLow = 1'000'000.0 / stats.p99FrameUs;

    // Stutter percentage: frames that take > 1.5x the mean frame time
    const double threshold = stats.meanFrameUs * 1.5;
    size_t stutters = 0;
    for (uint64_t t : m_frameTimesUs) {
        if (static_cast<double>(t) > threshold) ++stutters;
    }
    stats.stutterPct = (static_cast<double>(stutters) / stats.totalFrames) * 100.0;

    return stats;
}

// ─── GameOffloadAdapter Implementation ────────────────────────────────────────

struct GameOffloadAdapter::Impl {
    uint32_t width{};
    uint32_t height{};
    uint64_t currentTaskId = 7000;

    d3d12::GpuBuffer        readbackStaging;
    ID3D12Device5*          device{nullptr};

    task_runtime::JobHandle activeJob;
    bool hasActiveJob = false;
    bool lastWasRemote = false;
};

GameOffloadAdapter::GameOffloadAdapter()  : m_impl(std::make_unique<Impl>()) {}
GameOffloadAdapter::~GameOffloadAdapter() = default;

bool GameOffloadAdapter::init(ID3D12Device5* device, uint32_t width, uint32_t height) {
    m_impl->width  = width;
    m_impl->height = height;
    m_impl->device = device;

    if (device) {
        const uint64_t sz = static_cast<uint64_t>(width) * height * 4;
        if (!m_impl->readbackStaging.create(device, sz, d3d12::BufferUsage::Readback)) {
            std::cerr << "[GameOffloadAdapter] Failed to allocate D3D12 readback staging buffer\n";
            return false;
        }
    }
    return true;
}

bool GameOffloadAdapter::captureD3D12Texture(ID3D12GraphicsCommandList* cl,
                                            ID3D12Resource*             sourceResource,
                                            std::vector<std::byte>&     outPixels) {
    if (!cl || !sourceResource || !m_impl->readbackStaging.resource()) {
        return false;
    }

    // Copy source GPU render target resource into CPU-readable readback staging buffer
    cl->CopyResource(m_impl->readbackStaging.resource(), sourceResource);
    return m_impl->readbackStaging.readback(outPixels);
}

bool GameOffloadAdapter::submitPostProcess(const std::vector<std::byte>& intermediateFrame,
                                          uint32_t width,
                                          uint32_t height,
                                          host::JobScheduler& scheduler) {
    protocol::TaskRequest req;
    req.taskId = m_impl->currentTaskId++;
    req.type   = protocol::TaskType::Blur; // SOTTR Depth of Field / Bloom pass
    req.width  = width;
    req.height = height;
    req.input  = intermediateFrame;

    // Submit asynchronously through JobScheduler (uses adaptive site + circuit breaker)
    m_impl->activeJob = scheduler.submit(std::move(req));
    m_impl->hasActiveJob = true;
    m_impl->lastWasRemote = (m_impl->activeJob.site == task_runtime::ExecutionSite::Remote);
    return true;
}

bool GameOffloadAdapter::syncAndComposite(std::vector<std::byte>& backbuffer,
                                          uint32_t width,
                                          uint32_t height,
                                          host::FrameCompositor& compositor) {
    if (!m_impl->hasActiveJob) return false;

    // Await completed result at Present synchronization point
    auto result = m_impl->activeJob.future.get();
    m_impl->hasActiveJob = false;

    if (result.status != protocol::TaskStatus::Ok || result.output.empty()) {
        return false;
    }

    // Determine backbuffer dimensions
    uint32_t bbWidth = width;
    uint32_t bbHeight = height;
    if (backbuffer.size() != static_cast<size_t>(width) * height * 4 && (backbuffer.size() % 4 == 0)) {
        if (m_impl->width > 0 && m_impl->height > 0) {
            const double aspect = static_cast<double>(m_impl->width) / static_cast<double>(m_impl->height);
            size_t bbPixels = backbuffer.size() / 4;
            uint32_t deducedH = static_cast<uint32_t>(std::round(std::sqrt(bbPixels / aspect)));
            if (deducedH > 0 && (bbPixels % deducedH == 0)) {
                uint32_t deducedW = static_cast<uint32_t>(bbPixels / deducedH);
                if (static_cast<size_t>(deducedW) * deducedH * 4 == backbuffer.size()) {
                    bbWidth  = deducedW;
                    bbHeight = deducedH;
                }
            }
        }
    }

    if (bbWidth == m_impl->width && bbHeight == m_impl->height) {
        return compositor.composite(backbuffer, result.output, bbWidth, bbHeight,
                                    host::CompositeMode::AlphaBlend, 0.70f);
    } else {
        return compositor.compositeScaled(backbuffer, bbWidth, bbHeight,
                                          result.output, m_impl->width, m_impl->height,
                                          host::CompositeMode::AlphaBlend, 0.70f);
    }
}

bool GameOffloadAdapter::lastFrameWasRemote() const noexcept {
    return m_impl->lastWasRemote;
}

} // namespace dgpu::game_adapter
