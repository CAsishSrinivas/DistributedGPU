// apps/host/frame_compositor.cpp
// CPU-side frame compositor for M4 distributed rendering experiments.

#include "frame_compositor.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>

namespace dgpu::host {

struct FrameCompositor::Impl {};

FrameCompositor::FrameCompositor()  : m_impl(std::make_unique<Impl>()) {}
FrameCompositor::~FrameCompositor() = default;

static inline void sampleBilinearRGBA(const std::byte* remote,
                                       uint32_t rw, uint32_t rh,
                                       float u, float v,
                                       float outRgba[4]) {
    u = std::clamp(u, 0.0f, static_cast<float>(rw > 1 ? rw - 1 : 0));
    v = std::clamp(v, 0.0f, static_cast<float>(rh > 1 ? rh - 1 : 0));

    int x0 = static_cast<int>(std::floor(u));
    int y0 = static_cast<int>(std::floor(v));
    int x1 = std::min(x0 + 1, static_cast<int>(rw - 1));
    int y1 = std::min(y0 + 1, static_cast<int>(rh - 1));

    float fx = u - static_cast<float>(x0);
    float fy = v - static_cast<float>(y0);
    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = fx * (1.0f - fy);
    float w01 = (1.0f - fx) * fy;
    float w11 = fx * fy;

    auto fetch = [remote, rw](int x, int y, float p[4]) {
        size_t idx = (static_cast<size_t>(y) * rw + x) * 4;
        p[0] = static_cast<float>(static_cast<uint8_t>(remote[idx + 0]));
        p[1] = static_cast<float>(static_cast<uint8_t>(remote[idx + 1]));
        p[2] = static_cast<float>(static_cast<uint8_t>(remote[idx + 2]));
        p[3] = static_cast<float>(static_cast<uint8_t>(remote[idx + 3]));
    };

    float p00[4], p10[4], p01[4], p11[4];
    fetch(x0, y0, p00);
    fetch(x1, y0, p10);
    fetch(x0, y1, p01);
    fetch(x1, y1, p11);

    for (int c = 0; c < 4; ++c) {
        outRgba[c] = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
    }
}

bool FrameCompositor::compositeScaled(std::vector<std::byte>&       base,
                                      uint32_t                      baseWidth,
                                      uint32_t                      baseHeight,
                                      const std::vector<std::byte>& remote,
                                      uint32_t                      remoteWidth,
                                      uint32_t                      remoteHeight,
                                      CompositeMode                 mode,
                                      float                         alpha) {
    const size_t expectedBase   = static_cast<size_t>(baseWidth) * baseHeight * 4;
    const size_t expectedRemote = static_cast<size_t>(remoteWidth) * remoteHeight * 4;
    if (base.size() != expectedBase || remote.size() != expectedRemote) return false;
    if (baseWidth == 0 || baseHeight == 0 || remoteWidth == 0 || remoteHeight == 0) return false;

    // Fast path: if dimensions are identical, forward to direct 1:1 compositor
    if (baseWidth == remoteWidth && baseHeight == remoteHeight) {
        return composite(base, remote, baseWidth, baseHeight, mode, alpha);
    }

    const float scaleX = static_cast<float>(remoteWidth)  / static_cast<float>(baseWidth);
    const float scaleY = static_cast<float>(remoteHeight) / static_cast<float>(baseHeight);
    const float invAlpha = 1.0f - alpha;
    const uint32_t halfBaseWidth = baseWidth / 2;

    for (uint32_t by = 0; by < baseHeight; ++by) {
        float ry = (static_cast<float>(by) + 0.5f) * scaleY - 0.5f;
        for (uint32_t bx = 0; bx < baseWidth; ++bx) {
            if (mode == CompositeMode::SideBySide && bx < halfBaseWidth) {
                continue; // Left half keeps base unchanged
            }

            float rx = (static_cast<float>(bx) + 0.5f) * scaleX - 0.5f;
            float sampled[4];
            sampleBilinearRGBA(remote.data(), remoteWidth, remoteHeight, rx, ry, sampled);

            size_t bIdx = (static_cast<size_t>(by) * baseWidth + bx) * 4;

            if (mode == CompositeMode::Replace || mode == CompositeMode::SideBySide) {
                base[bIdx + 0] = static_cast<std::byte>(static_cast<uint8_t>(std::clamp(sampled[0], 0.0f, 255.0f)));
                base[bIdx + 1] = static_cast<std::byte>(static_cast<uint8_t>(std::clamp(sampled[1], 0.0f, 255.0f)));
                base[bIdx + 2] = static_cast<std::byte>(static_cast<uint8_t>(std::clamp(sampled[2], 0.0f, 255.0f)));
                // Alpha remains base
            } else if (mode == CompositeMode::AlphaBlend) {
                for (int c = 0; c < 3; ++c) {
                    float b = static_cast<float>(static_cast<uint8_t>(base[bIdx + c]));
                    float out = alpha * sampled[c] + invAlpha * b;
                    base[bIdx + c] = static_cast<std::byte>(
                        static_cast<uint8_t>(std::clamp(out, 0.0f, 255.0f)));
                }
            }
        }
    }

    return true;
}

bool FrameCompositor::composite(std::vector<std::byte>&       base,
                                const std::vector<std::byte>& remote,
                                uint32_t    width,
                                uint32_t    height,
                                CompositeMode mode,
                                float         alpha) {
    const size_t expected = static_cast<size_t>(width) * height * 4;

    // Detect general dimension mismatch and auto-convert
    if (base.size() != expected || remote.size() != expected) {
        if (width == 0 || height == 0) return false;
        const double aspect = static_cast<double>(width) / static_cast<double>(height);

        // Case A: (width, height) matches base; remote is scaled
        if (base.size() == expected && (remote.size() % 4 == 0) && !remote.empty()) {
            size_t remPixels = remote.size() / 4;
            uint32_t remH = static_cast<uint32_t>(std::round(std::sqrt(remPixels / aspect)));
            if (remH > 0 && (remPixels % remH == 0)) {
                uint32_t remW = static_cast<uint32_t>(remPixels / remH);
                if (static_cast<size_t>(remW) * remH * 4 == remote.size()) {
                    return compositeScaled(base, width, height, remote, remW, remH, mode, alpha);
                }
            }
        }

        // Case B: (width, height) matches remote; base is scaled
        if (remote.size() == expected && (base.size() % 4 == 0) && !base.empty()) {
            size_t basePixels = base.size() / 4;
            uint32_t baseH = static_cast<uint32_t>(std::round(std::sqrt(basePixels / aspect)));
            if (baseH > 0 && (basePixels % baseH == 0)) {
                uint32_t baseW = static_cast<uint32_t>(basePixels / baseH);
                if (static_cast<size_t>(baseW) * baseH * 4 == base.size()) {
                    return compositeScaled(base, baseW, baseH, remote, width, height, mode, alpha);
                }
            }
        }

        return false;
    }

    switch (mode) {

    case CompositeMode::Replace:
        std::memcpy(base.data(), remote.data(), expected);
        break;

    case CompositeMode::AlphaBlend: {
        const float inv = 1.0f - alpha;
        for (size_t i = 0; i < expected; i += 4) {
            for (int c = 0; c < 3; ++c) {
                float b = static_cast<float>(static_cast<uint8_t>(base[i + c]));
                float r = static_cast<float>(static_cast<uint8_t>(remote[i + c]));
                float out = alpha * r + inv * b;
                base[i + c] = static_cast<std::byte>(
                    static_cast<uint8_t>(std::clamp(out, 0.0f, 255.0f)));
            }
            // Alpha channel: keep base
        }
        break;
    }

    case CompositeMode::SideBySide: {
        // Left half: base (unmodified), right half: remote result
        const uint32_t half = width / 2;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = half; x < width; ++x) {
                size_t i = (static_cast<size_t>(y) * width + x) * 4;
                std::memcpy(&base[i], &remote[i], 4);
            }
        }
        break;
    }
    }

    return true;
}

double FrameCompositor::computeMAE(const std::vector<std::byte>& a,
                                    const std::vector<std::byte>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += std::abs(static_cast<int>(static_cast<uint8_t>(a[i]))
                      - static_cast<int>(static_cast<uint8_t>(b[i])));
    }
    return sum / static_cast<double>(a.size());
}

} // namespace dgpu::host
