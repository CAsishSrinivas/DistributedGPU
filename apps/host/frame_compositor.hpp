// apps/host/frame_compositor.hpp
// Combines the host-rendered image and the remote-processed result into
// the final frame (M4).
#pragma once

#include "protocol/message.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace dgpu::host {

// ─── Compositor modes ─────────────────────────────────────────────────────────

enum class CompositeMode {
    // Replace the host frame with the remote result (e.g. full post-process)
    Replace,

    // Alpha-blend remote result over the host frame
    AlphaBlend,

    // Side-by-side split for visual comparison during benchmarking
    SideBySide,
};

// ─── FrameCompositor ─────────────────────────────────────────────────────────
// Pure CPU compositor (M4).  A later version can move this to a D3D12
// compute shader for GPU-side compositing without a CPU round-trip.

class FrameCompositor {
public:
    FrameCompositor();
    ~FrameCompositor();

    FrameCompositor(const FrameCompositor&)            = delete;
    FrameCompositor& operator=(const FrameCompositor&) = delete;

    // Composite remote result into baseFrame in-place.
    // Both buffers must be RGBA8, row-major, same dimensions.
    [[nodiscard]] bool composite(std::vector<std::byte>&       baseFrame,
                                  const std::vector<std::byte>& remoteResult,
                                  uint32_t    width,
                                  uint32_t    height,
                                  CompositeMode mode  = CompositeMode::Replace,
                                  float         alpha = 1.0f);

    // Composite remote result into baseFrame with bilinear scaling.
    // Allows downsampled offload buffers (e.g. 960x540 bloom/DoF) over full-res (e.g. 1920x1080).
    [[nodiscard]] bool compositeScaled(std::vector<std::byte>&       baseFrame,
                                        uint32_t                      baseWidth,
                                        uint32_t                      baseHeight,
                                        const std::vector<std::byte>& remoteResult,
                                        uint32_t                      remoteWidth,
                                        uint32_t                      remoteHeight,
                                        CompositeMode                 mode  = CompositeMode::AlphaBlend,
                                        float                         alpha = 1.0f);

    // Verify two frames are within tolerance (acceptance test helper).
    // Returns mean absolute error per channel [0, 255].
    [[nodiscard]] static double computeMAE(const std::vector<std::byte>& a,
                                            const std::vector<std::byte>& b);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::host
