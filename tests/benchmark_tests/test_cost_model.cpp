// tests/benchmark_tests/test_cost_model.cpp
// Unit tests for WorkloadCostModel and multi-strategy scheduling selection.

#include "job_scheduler.hpp"
#include "protocol/message.hpp"
#include "workload_cost_model.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace dgpu;
    using namespace dgpu::host;

    std::cout << "[test_cost_model] Running WorkloadCostModel tests...\n";

    WorkloadCostModel model;

    // ── Test 1: Cold-Start Kernel Differentiation ────────────────────────────
    // At 1024x1024, Blur should take longer than EdgeDetection due to convolution radius
    auto evalBlur = model.evaluateStrategies(protocol::TaskType::Blur, 1024, 1024);
    auto evalEdge = model.evaluateStrategies(protocol::TaskType::EdgeDetection, 1024, 1024);

    assert(evalBlur.evaluatedStrategies.size() == 4);
    assert(evalEdge.evaluatedStrategies.size() == 4);
    assert(evalBlur.evaluatedStrategies[0].breakdown.gpuExecutionUs >
           evalEdge.evaluatedStrategies[0].breakdown.gpuExecutionUs);
    std::cout << "  [PASS] Cold-start kernel differentiation: Blur GPU ("
              << evalBlur.evaluatedStrategies[0].breakdown.gpuExecutionUs << " us) > Edge GPU ("
              << evalEdge.evaluatedStrategies[0].breakdown.gpuExecutionUs << " us)\n";

    // ── Test 2: Bandwidth Saturation and Automatic Local Fallback ────────────
    // Over a slow 100 Mbps link (12.5 MB/s) with 1024x1024 uncompressed (4 MB send + 4 MB recv = 8 MB),
    // wire transfer time (~640 ms) drastically exceeds local GPU execution (~25 ms).
    model.setLinkBandwidthBps(12'500'000); // 100 Mbps
    model.setCompressionEnabled(false);
    auto slowLinkEval = model.evaluateStrategies(protocol::TaskType::Blur, 1024, 1024);
    assert(slowLinkEval.isLocalFallback);
    assert(slowLinkEval.optimalStrategy == ExecutionStrategy::HostOnly);
    std::cout << "  [PASS] Slow link (100 Mbps, uncompressed) correctly triggered local fallback: "
              << slowLinkEval.decisionRationale << "\n";

    // ── Test 3: Fast Link Cooperative Partitioning ───────────────────────────
    // Over 2.5 Gbps link (~312.5 MB/s) with 4x compression, offloading or partitioning should be feasible.
    model.setLinkBandwidthBps(312'500'000); // 2.5 Gbps
    model.setCompressionEnabled(true);
    // Record empirical heavy compute samples (e.g. 4096x4096 heavy blur: Host 120ms, Worker 130ms)
    model.recordLocalSuccess(protocol::TaskType::Blur, 2048, 2048, 90'000, 90'000);
    model.recordRemoteSuccess(protocol::TaskType::Blur, 2048, 2048, 95'000, 20'000, 115'000, 16 * 1024 * 1024,
                              4 * 1024 * 1024);

    auto fastLinkEval = model.evaluateStrategies(protocol::TaskType::Blur, 2048, 2048);
    // Under 2.5 Gbps with heavy compute, partitioning or remote execution should be eligible
    assert(fastLinkEval.evaluatedStrategies[2].isEligible); // SpatiallyDistributed
    std::cout << "  [PASS] Fast link evaluation completed: Optimal=" << strategyToString(fastLinkEval.optimalStrategy)
              << " (Speedup=" << fastLinkEval.predictedSpeedupVsBaseline << "x)\n";

    // ── Test 4: Worker Capability Filtering & Ineligibility ──────────────────
    protocol::WorkerCapabilities caps;
    caps.workerId = "test-worker";
    caps.hostname = "WorkerB";
    caps.maxWidth = 1920;
    caps.maxHeight = 1080;
    caps.supportedTasks = protocol::TaskCap_Blur; // Only Blur supported, NOT EdgeDetection!
    model.setWorkerCapabilities(caps);

    // Case 4A: EdgeDetection should make WorkerOnly and SpatiallyDistributed ineligible
    auto edgeCapEval = model.evaluateStrategies(protocol::TaskType::EdgeDetection, 512, 512);
    assert(!edgeCapEval.evaluatedStrategies[1].isEligible); // WorkerOnly ineligible
    assert(!edgeCapEval.evaluatedStrategies[2].isEligible); // SpatiallyDistributed ineligible
    assert(edgeCapEval.optimalStrategy == ExecutionStrategy::HostOnly);
    std::cout << "  [PASS] Worker capability rejection for unsupported task verified: "
              << edgeCapEval.evaluatedStrategies[1].rejectionReason << "\n";

    // Case 4B: Resolution exceeding maxWidth (e.g. 2560x1440) should be rejected
    auto resCapEval = model.evaluateStrategies(protocol::TaskType::Blur, 2560, 1440);
    assert(!resCapEval.evaluatedStrategies[1].isEligible);
    assert(!resCapEval.evaluatedStrategies[2].isEligible);
    assert(resCapEval.optimalStrategy == ExecutionStrategy::HostOnly);
    std::cout << "  [PASS] Worker capability rejection for oversized resolution verified: "
              << resCapEval.evaluatedStrategies[1].rejectionReason << "\n";

    model.clearWorkerCapabilities();

    // ── Test 5: Dynamic Feedback Learning from Completed Jobs ────────────────
    WorkloadCostModel learningModel;
    learningModel.setLinkBandwidthBps(125'000'000); // 1 Gbps
    assert(learningModel.sampleCount(protocol::TaskType::Blur, 512, 512) == 0);

    // Initial baseline before samples
    auto initialEval = learningModel.evaluateStrategies(protocol::TaskType::Blur, 512, 512);
    double initialHostGpu = initialEval.evaluatedStrategies[0].breakdown.gpuExecutionUs;

    // Feed in 5 completed local executions of 8000 us
    for (int i = 0; i < 5; ++i) {
        learningModel.recordLocalSuccess(protocol::TaskType::Blur, 512, 512, 8'000, 8'500);
    }
    assert(learningModel.sampleCount(protocol::TaskType::Blur, 512, 512) == 5);

    auto updatedEval = learningModel.evaluateStrategies(protocol::TaskType::Blur, 512, 512);
    double updatedHostGpu = updatedEval.evaluatedStrategies[0].breakdown.gpuExecutionUs;
    assert(std::abs(updatedHostGpu - 8'000.0) < std::abs(initialHostGpu - 8'000.0));
    std::cout << "  [PASS] Dynamic feedback learning verified: Host GPU estimate calibrated from " << initialHostGpu
              << " us to " << updatedHostGpu << " us (target 8000 us)\n";

    // ── Test 6: Concurrent Batch Evaluation ──────────────────────────────────
    auto batchEval = learningModel.evaluateStrategies(protocol::TaskType::Blur, 512, 512, 0, 0, 0, true, 8);
    assert(batchEval.evaluatedStrategies[3].isEligible); // IndependentBatch eligible for batchSize=8
    assert(batchEval.evaluatedStrategies[3].breakdown.totalTurnaroundUs > 0.0);
    std::cout << "  [PASS] Concurrent batch makespan evaluation verified for batchSize=8: "
              << batchEval.evaluatedStrategies[3].breakdown.totalTurnaroundUs << " us\n";

    // ── Test 7: JobScheduler Integration ─────────────────────────────────────
    JobScheduler scheduler;
    scheduler.setLinkBandwidthBps(12'500'000); // 100 Mbps
    protocol::TaskRequest req;
    req.taskId = 999;
    req.type = protocol::TaskType::Blur;
    req.width = 1024;
    req.height = 1024;
    req.input.resize(1024 * 1024 * 4);

    auto schedComp = scheduler.evaluateStrategies(req);
    assert(schedComp.optimalStrategy == ExecutionStrategy::HostOnly);
    assert(schedComp.isLocalFallback);
    std::cout << "  [PASS] JobScheduler cost model evaluation verified: " << schedComp.decisionRationale << "\n";

    std::cout << "\n[test_cost_model] ALL 7 TESTS PASSED SUCCESSFULLY.\n";
    return 0;
}
