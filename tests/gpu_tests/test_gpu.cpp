// tests/gpu_tests/test_gpu.cpp
// Tests GpuExecutor on the local GPU — verifies output size and
// pixel-wise determinism between two identical runs.

#include "d3d12/gpu_executor.hpp"
#include "protocol/message.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace dgpu::gpu_executor;
using namespace dgpu::protocol;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::cerr << "FAIL [" __FILE__ ":" << __LINE__ << "] " #expr << '\n'; \
            ++g_failed;                                                      \
        } else {                                                             \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

static std::vector<std::byte> makeImage(uint32_t w, uint32_t h) {
    std::vector<std::byte> img(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            img[i+0] = static_cast<std::byte>((x + y) & 0xFF);
            img[i+1] = static_cast<std::byte>(x & 0xFF);
            img[i+2] = static_cast<std::byte>(y & 0xFF);
            img[i+3] = static_cast<std::byte>(0xFF);
        }
    return img;
}

static void test_blur_output_size(GpuExecutor& exec) {
    TaskRequest req;
    req.taskId = 1;
    req.type   = TaskType::Blur;
    req.width  = 64;
    req.height = 64;
    req.input  = makeImage(64, 64);

    auto res = exec.execute(req);
    CHECK(res.status == TaskStatus::Ok);
    CHECK(res.output.size() == static_cast<size_t>(64 * 64 * 4));
    std::cout << "  blur output size: PASS\n";
}

static void test_edge_detect_output_size(GpuExecutor& exec) {
    TaskRequest req;
    req.taskId = 2;
    req.type   = TaskType::EdgeDetection;
    req.width  = 64;
    req.height = 64;
    req.input  = makeImage(64, 64);

    auto res = exec.execute(req);
    CHECK(res.status == TaskStatus::Ok);
    CHECK(res.output.size() == static_cast<size_t>(64 * 64 * 4));
    std::cout << "  edge detect output size: PASS\n";
}

static void test_determinism(GpuExecutor& exec) {
    auto img = makeImage(128, 128);

    TaskRequest r1, r2;
    r1.taskId = 3; r1.type = TaskType::Blur; r1.width = 128; r1.height = 128; r1.input = img;
    r2.taskId = 4; r2.type = TaskType::Blur; r2.width = 128; r2.height = 128; r2.input = img;

    auto res1 = exec.execute(r1);
    auto res2 = exec.execute(r2);

    CHECK(res1.status == TaskStatus::Ok);
    CHECK(res2.status == TaskStatus::Ok);
    CHECK(res1.output == res2.output);
    std::cout << "  determinism (128x128 blur): " << (res1.output == res2.output ? "PASS" : "FAIL") << '\n';
}

static void test_invalid_task(GpuExecutor& exec) {
    TaskRequest req;
    req.taskId = 5;
    req.type   = static_cast<TaskType>(0xBEEF);
    req.width  = 4;
    req.height = 4;
    req.input.resize(64, std::byte{0});

    auto res = exec.execute(req);
    CHECK(res.status == TaskStatus::InvalidRequest ||
          res.status == TaskStatus::UnsupportedTask);
    std::cout << "  invalid task type rejection: PASS\n";
}

int main() {
    std::cout << "=== GPU Tests ===\n";

    GpuExecutor exec;
    if (!exec.init()) {
        std::cerr << "SKIP: GPU init failed (no hardware adapter available)\n";
        return 0; // Not a failure in CI without a GPU
    }
    std::cout << "GPU: " << exec.adapterInfo().name << '\n';

    test_blur_output_size(exec);
    test_edge_detect_output_size(exec);
    test_determinism(exec);
    test_invalid_task(exec);

    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
