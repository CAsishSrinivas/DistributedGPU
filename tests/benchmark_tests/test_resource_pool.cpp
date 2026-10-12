// tests/benchmark_tests/test_resource_pool.cpp
// Comprehensive unit test suite for Phase 2.4:
// D3D12 Resource and Memory Reuse (Fence-Synchronized Buffer & Allocator Pooling).

#include "d3d12/resource_pool.hpp"
#include "d3d12/pipelined_gpu_executor.hpp"
#include "d3d12/gpu_executor.hpp"
#include "apps/host/frame_compositor.hpp"
#include "protocol/message.hpp"

#include <iostream>
#include <vector>

using namespace dgpu::d3d12;
using namespace dgpu::gpu_executor;
using namespace dgpu::protocol;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << #expr << '\n'; \
            ++g_failed;                                                                \
        } else {                                                                       \
            ++g_passed;                                                                \
        }                                                                              \
    } while (0)

static std::vector<std::byte> makePattern(uint32_t w, uint32_t h) {
    std::vector<std::byte> data(static_cast<uint64_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            data[idx + 0] = static_cast<std::byte>((x + y) & 0xFF);
            data[idx + 1] = static_cast<std::byte>((x * 3) & 0xFF);
            data[idx + 2] = static_cast<std::byte>((y * 7) & 0xFF);
            data[idx + 3] = static_cast<std::byte>(0xFF);
        }
    }
    return data;
}

// ─── Test 1: Buffer Acquisition, Fence Lifecycle, and Recycling ──────────────

static void test_buffer_pool_lifecycle(ID3D12Device5* device) {
    std::cout << "[Test 1] Testing Buffer Acquisition & Fence-Synchronized Recycling...\n";

    D3D12ResourcePool pool(512ull * 1024ull * 1024ull);

    const uint64_t sz = 1024 * 1024 * 4; // 4 MB

    // 1. Initial acquisition (pool miss)
    auto* b1 = pool.acquireBuffer(device, sz, BufferUsage::Default, 0);
    CHECK(b1 != nullptr);
    CHECK(b1->inUse == true);
    CHECK(pool.metrics().poolMisses == 1);
    CHECK(pool.metrics().poolHits == 0);

    // 2. Release with future completion fence = 5
    pool.releaseBuffer(b1, 5);
    CHECK(b1->inUse == false);
    CHECK(b1->lastFenceValue == 5);

    // 3. Try to acquire when completed fence is only 3 (should NOT reuse b1 yet)
    auto* b2 = pool.acquireBuffer(device, sz, BufferUsage::Default, 3);
    CHECK(b2 != nullptr);
    CHECK(b2 != b1); // allocated a second buffer because b1 is still in flight
    CHECK(pool.metrics().poolMisses == 2);

    pool.releaseBuffer(b2, 10);

    // 4. Try to acquire when completed fence is 6 (b1 is now safe to reuse!)
    auto* b3 = pool.acquireBuffer(device, sz, BufferUsage::Default, 6);
    CHECK(b3 != nullptr);
    CHECK(b3 == b1); // b1 recycled!
    CHECK(pool.metrics().poolHits == 1);
    CHECK(pool.metrics().totalAllocations == 2);

    pool.releaseBuffer(b3, 12);

    std::cout << "  Passed (Pool hits: " << pool.metrics().poolHits
              << ", Total allocated: " << pool.metrics().totalAllocatedBytes / (1024 * 1024) << " MB)\n";
}

// ─── Test 2: Best-Fit Bucket Sizing ──────────────────────────────────────────

static void test_best_fit_bucket_sizing(ID3D12Device5* device) {
    std::cout << "[Test 2] Testing Best-Fit Bucket Sizing...\n";

    D3D12ResourcePool pool(512ull * 1024ull * 1024ull);

    // Allocate 64KB, 256KB, 1MB buffers
    auto* b64k  = pool.acquireBuffer(device, 64 * 1024, BufferUsage::Upload, 0);
    auto* b256k = pool.acquireBuffer(device, 256 * 1024, BufferUsage::Upload, 0);
    auto* b1m   = pool.acquireBuffer(device, 1024 * 1024, BufferUsage::Upload, 0);

    CHECK(b64k != nullptr);
    CHECK(b256k != nullptr);
    CHECK(b1m != nullptr);

    pool.releaseBuffer(b64k, 1);
    pool.releaseBuffer(b256k, 1);
    pool.releaseBuffer(b1m, 1);

    // Request 128KB: best-fit should pick the 256KB buffer, NOT the 1MB buffer!
    auto* bPicked = pool.acquireBuffer(device, 128 * 1024, BufferUsage::Upload, 1);
    CHECK(bPicked == b256k);
    CHECK(pool.metrics().poolHits == 1);

    pool.releaseBuffer(bPicked, 2);

    std::cout << "  Passed (Best-fit correctly selected 256KB buffer over 1MB buffer)\n";
}

// ─── Test 3: VRAM Budget Enforcement & Trimming ──────────────────────────────

static void test_vram_budget_and_trimming(ID3D12Device5* device) {
    std::cout << "[Test 3] Testing VRAM Budget Enforcement & Trimming...\n";

    // Set tight 16 MB budget
    const uint64_t budget = 16ull * 1024ull * 1024ull;
    D3D12ResourcePool pool(budget);

    CHECK(pool.vramBudgetBytes() == budget);

    // Allocate 3 buffers of 4 MB each (total 12 MB)
    auto* b1 = pool.acquireBuffer(device, 4 * 1024 * 1024, BufferUsage::Default, 0);
    auto* b2 = pool.acquireBuffer(device, 4 * 1024 * 1024, BufferUsage::Default, 0);
    auto* b3 = pool.acquireBuffer(device, 4 * 1024 * 1024, BufferUsage::Default, 0);

    CHECK(pool.activeVramBytes() >= 12 * 1024 * 1024);
    CHECK(pool.activeVramBytes() <= budget);

    pool.releaseBuffer(b1, 5);
    pool.releaseBuffer(b2, 5);
    pool.releaseBuffer(b3, 5);

    // Trim all idle buffers completed by fence 5
    pool.trim(5);
    CHECK(pool.activeVramBytes() == 0);
    CHECK(pool.metrics().activeAllocations == 0);

    std::cout << "  Passed (Active VRAM trimmed cleanly from 12 MB down to 0 MB)\n";
}

// ─── Test 4: Command Allocator Pooling ────────────────────────────────────────

static void test_command_allocator_pooling(ID3D12Device5* device) {
    std::cout << "[Test 4] Testing Command Allocator Pooling...\n";

    D3D12ResourcePool pool;

    auto* a1 = pool.acquireCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, 0);
    auto* a2 = pool.acquireCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, 0);
    CHECK(a1 != nullptr);
    CHECK(a2 != nullptr);
    CHECK(a1 != a2);

    pool.releaseCommandAllocator(a1, 10);
    pool.releaseCommandAllocator(a2, 20);

    // Re-acquire when completed fence is 15 (a1 is completed and should be reused)
    auto* a3 = pool.acquireCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, 15);
    CHECK(a3 == a1);

    pool.releaseCommandAllocator(a3, 30);

    std::cout << "  Passed (Command allocator safely recycled after fence completion)\n";
}

// ─── Test 5: Zero Steady-State Allocations in Pipelined GpuExecutor ───────────

static void test_steady_state_zero_allocations() {
    std::cout << "[Test 5] Testing Steady-State Zero-Allocation Guarantee with PipelinedGpuExecutor...\n";

    PipelinedGpuExecutor exec;
    if (!exec.init("", 2)) {
        std::cout << "  [SKIP] D3D12 device not available\n";
        return;
    }

    const uint32_t w = 128;
    const uint32_t h = 128;
    const size_t warmupCount = 4;
    const size_t steadyStateCount = 12;

    std::vector<TaskRequest> warmupTasks;
    for (size_t i = 0; i < warmupCount; ++i) {
        TaskRequest req;
        req.taskId = 10 + i;
        req.type = TaskType::EdgeDetection;
        req.width = w;
        req.height = h;
        req.input = makePattern(w, h);
        warmupTasks.push_back(std::move(req));
    }

    // Warmup run to populate the resource pool
    auto warmupResults = exec.executePipelined(warmupTasks);
    CHECK(warmupResults.size() == warmupCount);

    // Initial allocations recorded
    auto initialMetrics = exec.resourcePoolMetrics();
    CHECK(initialMetrics.totalAllocations > 0);

    // Reset hit/miss counters to measure steady-state strictly
    exec.resetPoolMetrics();

    std::vector<TaskRequest> steadyTasks;
    for (size_t i = 0; i < steadyStateCount; ++i) {
        TaskRequest req;
        req.taskId = 100 + i;
        req.type = TaskType::EdgeDetection;
        req.width = w;
        req.height = h;
        req.input = makePattern(w, h);
        steadyTasks.push_back(std::move(req));
    }

    PipelinedExecutionMetrics pipeMetrics;
    auto steadyResults = exec.executePipelined(steadyTasks, &pipeMetrics);

    CHECK(steadyResults.size() == steadyStateCount);
    for (size_t i = 0; i < steadyStateCount; ++i) {
        CHECK(steadyResults[i].status == TaskStatus::Ok);
    }

    auto steadyPoolMetrics = exec.resourcePoolMetrics();

    // STEADY STATE ACCEPTANCE CRITERIA:
    // Zero new committed allocations during steady-state!
    // All requests must be 100% pool hits!
    CHECK(steadyPoolMetrics.poolMisses == 0);
    CHECK(steadyPoolMetrics.poolHits > 0);
    CHECK(steadyPoolMetrics.hitRatio == 1.0);

    std::cout << "  Passed (Steady-state Pool Hits: " << steadyPoolMetrics.poolHits
              << ", Pool Misses: " << steadyPoolMetrics.poolMisses
              << ", Hit Ratio: " << steadyPoolMetrics.hitRatio * 100.0 << "%"
              << ", Zero New Allocations!)\n";
}

int main() {
    std::cout << "====================================================\n";
    std::cout << "  DistributedGPU Phase 2.4: Resource Pool Tests\n";
    std::cout << "====================================================\n";

    D3D12Context ctx;
    if (ctx.init("")) {
        test_buffer_pool_lifecycle(ctx.device());
        test_best_fit_bucket_sizing(ctx.device());
        test_vram_budget_and_trimming(ctx.device());
        test_command_allocator_pooling(ctx.device());
        test_steady_state_zero_allocations();
    } else {
        std::cout << "  [SKIP] No D3D12 hardware device available\n";
    }

    std::cout << "====================================================\n";
    std::cout << "  Results: " << g_passed << " passed, " << g_failed << " failed\n";
    std::cout << "====================================================\n";

    return (g_failed == 0) ? 0 : 1;
}
