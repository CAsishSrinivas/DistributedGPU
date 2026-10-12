// tests/benchmark_tests/test_compositor.cpp
// Unit tests for FrameCompositor: 1:1 compositing, bilinear scaled compositing,
// and resolution mismatch handling.

#include "../../apps/host/frame_compositor.hpp"

#include <cmath>
#include <iostream>
#include <vector>

using namespace dgpu::host;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::cerr << "FAIL [" __FILE__ ":" << __LINE__ << "] " #expr << '\n'; \
            ++g_failed;                                                        \
        } else {                                                               \
            ++g_passed;                                                        \
        }                                                                      \
    } while (0)

static void test_1to1_composite() {
    FrameCompositor comp;
    const uint32_t w = 4;
    const uint32_t h = 4;
    const size_t sz = w * h * 4;

    std::vector<std::byte> base(sz, std::byte{100});
    std::vector<std::byte> remote(sz, std::byte{200});

    bool ok = comp.composite(base, remote, w, h, CompositeMode::Replace);
    CHECK(ok);
    CHECK(base[0] == std::byte{200});
    CHECK(base[sz - 1] == std::byte{200});
}

static void test_scaled_composite_1080p_540p() {
    FrameCompositor comp;
    const uint32_t baseW = 1920;
    const uint32_t baseH = 1080;
    const uint32_t remoteW = 960;
    const uint32_t remoteH = 540;

    std::vector<std::byte> base(baseW * baseH * 4, std::byte{50});
    std::vector<std::byte> remote(remoteW * remoteH * 4, std::byte{150});

    // Explicit scaled composite
    bool ok = comp.compositeScaled(base, baseW, baseH, remote, remoteW, remoteH,
                                   CompositeMode::AlphaBlend, 0.5f);
    CHECK(ok);
    // 0.5 * 150 + 0.5 * 50 = 100
    uint8_t val = static_cast<uint8_t>(base[0]);
    CHECK(val >= 98 && val <= 102);

    // Auto-detect resolution mismatch path in composite()
    std::vector<std::byte> base2(baseW * baseH * 4, std::byte{50});
    bool autoOk = comp.composite(base2, remote, remoteW, remoteH,
                                 CompositeMode::AlphaBlend, 0.5f);
    CHECK(autoOk);
    uint8_t val2 = static_cast<uint8_t>(base2[0]);
    CHECK(val2 >= 98 && val2 <= 102);
}

static void test_corrupted_size_rejected() {
    FrameCompositor comp;
    std::vector<std::byte> base(100, std::byte{0});
    std::vector<std::byte> remote(200, std::byte{0});

    bool ok = comp.composite(base, remote, 10, 10);
    CHECK(!ok);
}

static void test_general_scaled_composite_720p_360p() {
    FrameCompositor comp;
    const uint32_t baseW = 1280;
    const uint32_t baseH = 720;
    const uint32_t remoteW = 640;
    const uint32_t remoteH = 360;

    std::vector<std::byte> base(baseW * baseH * 4, std::byte{40});
    std::vector<std::byte> remote(remoteW * remoteH * 4, std::byte{160});

    // Test automatic dimension deduction with 720p base and 360p remote
    bool autoOk = comp.composite(base, remote, baseW, baseH, CompositeMode::AlphaBlend, 0.5f);
    CHECK(autoOk);
    uint8_t val = static_cast<uint8_t>(base[0]);
    CHECK(val >= 98 && val <= 102);
}

int main() {
    std::cout << "=== Compositor Tests ===\n";
    test_1to1_composite();
    test_scaled_composite_1080p_540p();
    test_general_scaled_composite_720p_360p();
    test_corrupted_size_rejected();

    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
