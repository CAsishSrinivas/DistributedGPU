// apps/game_adapter/d3d12_game_hook.hpp
// Milestone M7: Game-specific D3D12 offload adapter and frame pacer.
// Intercepts intermediate render targets, offloads post-processing passes to
// the remote worker (GTX 1650 Ti), and synchronises at swapchain Present.
#pragma once

#include <d3d12.h>
#include "protocol/message.hpp"
#include "d3d12/d3d12_context.hpp"
#include "task_runtime/task_runtime.hpp"
#include "telemetry/telemetry.hpp"

#include "../host/job_scheduler.hpp"
#include "../host/frame_compositor.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace dgpu::game_adapter {

// ─── Frame Pacing & Performance Metrics (M7) ──────────────────────────────────

struct FramePacingStats {
    double   avgFps        = 0.0;
    double   onePercentLow = 0.0;
    double   meanFrameUs   = 0.0;
    double   p99FrameUs    = 0.0;
    double   stutterPct    = 0.0; // % of frames deviating >50% from mean
    uint64_t totalFrames   = 0;
};

class FramePacer {
public:
    void recordFrame(uint64_t frameTimeUs);
    [[nodiscard]] FramePacingStats computeStats() const;
    void reset();

private:
    std::vector<uint64_t> m_frameTimesUs;
};

// ─── Game Offload Adapter ─────────────────────────────────────────────────────

class GameOffloadAdapter {
public:
    GameOffloadAdapter();
    ~GameOffloadAdapter();

    GameOffloadAdapter(const GameOffloadAdapter&)            = delete;
    GameOffloadAdapter& operator=(const GameOffloadAdapter&) = delete;

    // Initialise staging resources for target offload resolution
    [[nodiscard]] bool init(ID3D12Device5* device, uint32_t width, uint32_t height);

    // Extracts GPU texture data directly from D3D12 render target/intermediate resource
    [[nodiscard]] bool captureD3D12Texture(ID3D12GraphicsCommandList* cl,
                                          ID3D12Resource*             sourceResource,
                                          std::vector<std::byte>&     outPixels);

    // Asynchronously submit current frame's intermediate render target to worker
    [[nodiscard]] bool submitPostProcess(const std::vector<std::byte>& intermediateFrame,
                                          uint32_t width,
                                          uint32_t height,
                                          host::JobScheduler& scheduler);

    // Synchronise and composite result into game backbuffer at Present
    [[nodiscard]] bool syncAndComposite(std::vector<std::byte>& backbuffer,
                                         uint32_t width,
                                         uint32_t height,
                                         host::FrameCompositor& compositor);

    // Returns whether last frame was processed remotely or fell back to local
    [[nodiscard]] bool lastFrameWasRemote() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::game_adapter
