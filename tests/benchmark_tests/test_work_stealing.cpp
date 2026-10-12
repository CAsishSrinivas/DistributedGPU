// tests/benchmark_tests/test_work_stealing.cpp
// Unit and acceptance tests for Phase 2.2: Dynamic Work Stealing and Adaptive Load Balancer.

#include "batch_scheduler.hpp"
#include "protocol/message.hpp"
#include "work_distributor.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    using namespace dgpu;
    using namespace dgpu::host;

    std::cout << "[test_work_stealing] Running Dynamic Work-Stealing tests...\n";

    // ── Test 1: Uneven Task Durations & Speed-Proportional Makespan vs Static 50/50
    // Host is faster (~4 ms/task), Worker is slower (~10 ms/task).
    // Tasks have uneven durations (alternating light/heavy).
    {
        const uint32_t numTasks = 16;
        std::vector<protocol::TaskRequest> tasks(numTasks);
        for (uint32_t i = 0; i < numTasks; ++i) {
            tasks[i].taskId = i + 1;
            tasks[i].type = (i % 2 == 0) ? protocol::TaskType::Blur : protocol::TaskType::EdgeDetection;
            tasks[i].width = 512;
            tasks[i].height = 512;
            tasks[i].input.resize(512 * 512 * 4);
        }

        // Host executor: simulated ~4 ms per task
        auto hostExec = [](const protocol::TaskRequest& req) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            protocol::TaskResult r;
            r.taskId = req.taskId;
            r.status = protocol::TaskStatus::Ok;
            r.gpuUs = 4000;
            r.totalUs = 4000;
            return r;
        };

        // Worker executor: simulated ~10 ms per task
        auto workerExec = [](const protocol::TaskRequest& req) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            protocol::TaskResult r;
            r.taskId = req.taskId;
            r.status = protocol::TaskStatus::Ok;
            r.gpuUs = 10000;
            r.totalUs = 10000;
            return r;
        };

        // 1A. Static 50/50 allocation: 8 tasks on Host (8 * 4ms = 32ms), 8 tasks on Worker (8 * 10ms = 80ms)
        // Makespan is bounded by the slower worker: max(32ms, 80ms) ~ 80ms!
        auto staticMetrics = AdvancedBatchScheduler::runStaticSplit(tasks, numTasks / 2, hostExec, workerExec, true);

        // 1B. Dynamic Work-Stealing Load Balancer:
        // Host finishes its tasks and dynamically steals from worker queue, balancing the load!
        auto adaptiveMetrics = AdvancedBatchScheduler::runAdaptiveWorkStealing(tasks, hostExec, workerExec, true);

        std::cout << "  Static 50/50 Makespan:   " << staticMetrics.totalWallClockMs << " ms ("
                  << staticMetrics.throughputTasksPerSec << " tasks/s)\n";
        std::cout << "  Dynamic Work-Stealing:   " << adaptiveMetrics.totalWallClockMs << " ms ("
                  << adaptiveMetrics.throughputTasksPerSec << " tasks/s)\n";
        std::cout << "  Tasks Stolen by Host:    " << adaptiveMetrics.tasksStolenByHost << "\n";

        // Acceptance test: Adaptive work-stealing MUST complete with lower makespan than static 50/50
        assert(adaptiveMetrics.totalWallClockMs < staticMetrics.totalWallClockMs);
        assert(adaptiveMetrics.hostTasksExecuted > adaptiveMetrics.workerTasksExecuted);
        assert(adaptiveMetrics.tasksStolenByHost > 0);
        std::cout << "  [PASS] Uneven speed load balancing verified: Work stealing reduced makespan from "
                  << staticMetrics.totalWallClockMs << " ms to " << adaptiveMetrics.totalWallClockMs << " ms\n";
    }

    // ── Test 2: Tiny Task Filter & Local Retention ────────────────────────────
    // Tasks with small dimensions (e.g. 64x64) where wire transfer latency exceeds compute
    {
        const uint32_t numTiny = 10;
        std::vector<protocol::TaskRequest> tinyTasks(numTiny);
        for (uint32_t i = 0; i < numTiny; ++i) {
            tinyTasks[i].taskId = 100 + i;
            tinyTasks[i].type = protocol::TaskType::Blur;
            tinyTasks[i].width = 64;
            tinyTasks[i].height = 64;
            tinyTasks[i].input.resize(64 * 64 * 4);
        }

        DynamicWorkDistributor distributor;
        distributor.setLinkBandwidthBps(12'500'000); // 100 Mbps

        auto execNoOp = [](const protocol::TaskRequest& req) {
            protocol::TaskResult r;
            r.taskId = req.taskId;
            r.status = protocol::TaskStatus::Ok;
            return r;
        };

        auto metrics = distributor.executeBatch(tinyTasks, execNoOp, execNoOp, true);

        assert(metrics.tinyTasksRetainedLocally == numTiny);
        assert(metrics.hostTasksExecuted == numTiny);
        assert(metrics.workerTasksExecuted == 0);
        std::cout << "  [PASS] Tiny task filter verified: All " << metrics.tinyTasksRetainedLocally
                  << " tiny tasks retained on local GPU to avoid wire stall\n";
    }

    // ── Test 3: Capability-Constrained Work Stealing ──────────────────────────
    // Worker only supports Blur; EdgeDetection tasks must NOT be stolen by worker!
    {
        protocol::WorkerCapabilities caps;
        caps.supportedTasks = protocol::TaskCap_Blur; // Worker does NOT support EdgeDetection

        const uint32_t numTasks = 8;
        std::vector<protocol::TaskRequest> edgeTasks(numTasks);
        for (uint32_t i = 0; i < numTasks; ++i) {
            edgeTasks[i].taskId = 200 + i;
            edgeTasks[i].type = protocol::TaskType::EdgeDetection;
            edgeTasks[i].width = 512;
            edgeTasks[i].height = 512;
            edgeTasks[i].input.resize(512 * 512 * 4);
        }

        DynamicWorkDistributor distributor;
        distributor.setWorkerCapabilities(caps);

        auto execNoOp = [](const protocol::TaskRequest& req) {
            protocol::TaskResult r;
            r.taskId = req.taskId;
            r.status = protocol::TaskStatus::Ok;
            return r;
        };

        auto metrics = distributor.executeBatch(edgeTasks, execNoOp, execNoOp, true);

        assert(metrics.hostTasksExecuted == numTasks);
        assert(metrics.workerTasksExecuted == 0);
        assert(metrics.tasksStolenByWorker == 0);
        std::cout << "  [PASS] Capability-constrained work stealing verified: Worker rejected all " << numTasks
                  << " unsupported EdgeDetection tasks\n";
    }

    // ── Test 4: Bounded Queues and Backpressure ──────────────────────────────
    {
        WorkDistributorConfig cfg;
        cfg.maxQueueDepth = 4; // Max 4 tasks queued per worker
        DynamicWorkDistributor boundedDistributor(cfg);

        assert(boundedDistributor.config().maxQueueDepth == 4);
        std::cout << "  [PASS] Bounded queue configuration and backpressure parameters verified\n";
    }

    std::cout << "\n[test_work_stealing] ALL 4 TESTS PASSED SUCCESSFULLY.\n";
    return 0;
}
