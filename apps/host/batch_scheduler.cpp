// apps/host/batch_scheduler.cpp
// Advanced batch scheduling system implementation.

#include "batch_scheduler.hpp"
#include "frame_compositor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <mutex>
#include <numeric>
#include <thread>

namespace dgpu::host {

static double computePercentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::ceil(p * v.size())) - 1;
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

BatchRunMetrics AdvancedBatchScheduler::runHostOnly(
    const std::vector<protocol::TaskRequest>& tasks,
    LocalExecFn localExec) {

    BatchRunMetrics m;
    m.modeName = "Host RTX 3050 Alone";
    m.totalTasks = static_cast<uint32_t>(tasks.size());
    m.hostTasksExecuted = m.totalTasks;
    m.workerTasksExecuted = 0;
    m.taskDetails.resize(m.totalTasks);

    std::vector<double> latencies;
    latencies.reserve(m.totalTasks);

    auto batchStart = std::chrono::steady_clock::now();

    for (size_t i = 0; i < tasks.size(); ++i) {
        auto taskStart = std::chrono::steady_clock::now();
        double waitMs = std::chrono::duration<double, std::milli>(taskStart - batchStart).count();

        auto res = localExec(tasks[i]);
        auto taskEnd = std::chrono::steady_clock::now();
        double taskMs = std::chrono::duration<double, std::milli>(taskEnd - taskStart).count();

        latencies.push_back(taskMs);

        m.taskDetails[i].taskId = tasks[i].taskId;
        m.taskDetails[i].executedOn = "Host (RTX 3050)";
        m.taskDetails[i].queueWaitMs = waitMs;
        m.taskDetails[i].gpuExecutionMs = static_cast<double>(res.gpuUs) / 1000.0;
        m.taskDetails[i].totalTurnaroundMs = taskMs;
        m.taskDetails[i].success = (res.status == protocol::TaskStatus::Ok);
        if (!m.taskDetails[i].success) ++m.failedTasks;
    }

    auto batchEnd = std::chrono::steady_clock::now();
    m.totalWallClockMs = std::chrono::duration<double, std::milli>(batchEnd - batchStart).count();
    m.throughputTasksPerSec = (m.totalTasks * 1000.0) / (m.totalWallClockMs > 0 ? m.totalWallClockMs : 1.0);
    m.speedupVsHostAlone = 1.00;

    m.p50LatencyMs = computePercentile(latencies, 0.50);
    m.p90LatencyMs = computePercentile(latencies, 0.90);
    m.p95LatencyMs = computePercentile(latencies, 0.95);
    m.p99LatencyMs = computePercentile(latencies, 0.99);
    m.maxLatencyMs = latencies.empty() ? 0.0 : *std::max_element(latencies.begin(), latencies.end());

    return m;
}

BatchRunMetrics AdvancedBatchScheduler::runStaticSplit(
    const std::vector<protocol::TaskRequest>& tasks,
    uint32_t hostTaskCount,
    LocalExecFn localExec,
    RemoteExecFn remoteExec,
    bool remoteAvailable,
    double referenceHostThroughput,
    const std::vector<protocol::TaskResult>& referenceResults) {

    uint32_t n = static_cast<uint32_t>(tasks.size());
    hostTaskCount = std::min(hostTaskCount, n);
    uint32_t workerTaskCount = n - hostTaskCount;

    BatchRunMetrics m;
    m.modeName = std::format("Static Split ({}/{} Host/Worker)", hostTaskCount, workerTaskCount);
    m.totalTasks = n;
    m.hostTasksExecuted = hostTaskCount;
    m.workerTasksExecuted = workerTaskCount;
    m.taskDetails.resize(n);

    std::vector<protocol::TaskResult> results(n);
    std::vector<double> latencies(n, 0.0);

    auto batchStart = std::chrono::steady_clock::now();

    // Host thread executes tasks [0, hostTaskCount)
    auto runHost = [&]() {
        for (uint32_t i = 0; i < hostTaskCount; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            double waitMs = std::chrono::duration<double, std::milli>(t0 - batchStart).count();
            results[i] = localExec(tasks[i]);
            auto t1 = std::chrono::steady_clock::now();
            double durMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

            latencies[i] = durMs;
            m.taskDetails[i].taskId = tasks[i].taskId;
            m.taskDetails[i].executedOn = "Host (RTX 3050)";
            m.taskDetails[i].queueWaitMs = waitMs;
            m.taskDetails[i].gpuExecutionMs = static_cast<double>(results[i].gpuUs) / 1000.0;
            m.taskDetails[i].totalTurnaroundMs = durMs;
            m.taskDetails[i].success = (results[i].status == protocol::TaskStatus::Ok);
        }
    };

    // Worker thread executes tasks [hostTaskCount, n)
    auto runWorker = [&]() {
        for (uint32_t i = hostTaskCount; i < n; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            double waitMs = std::chrono::duration<double, std::milli>(t0 - batchStart).count();
            if (remoteAvailable && remoteExec) {
                results[i] = remoteExec(tasks[i]);
            } else {
                results[i] = localExec(tasks[i]);
            }
            auto t1 = std::chrono::steady_clock::now();
            double durMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

            latencies[i] = durMs;
            m.taskDetails[i].taskId = tasks[i].taskId;
            m.taskDetails[i].executedOn = remoteAvailable ? "Worker (GTX 1650 Ti)" : "Host (Fallback)";
            m.taskDetails[i].queueWaitMs = waitMs;
            m.taskDetails[i].gpuExecutionMs = static_cast<double>(results[i].gpuUs) / 1000.0;
            m.taskDetails[i].totalTurnaroundMs = durMs;
            m.taskDetails[i].success = (results[i].status == protocol::TaskStatus::Ok);
        }
    };

    if (hostTaskCount == n) {
        runHost();
    } else if (hostTaskCount == 0) {
        runWorker();
    } else {
        std::thread thHost(runHost);
        std::thread thWorker(runWorker);
        thHost.join();
        thWorker.join();
    }

    auto batchEnd = std::chrono::steady_clock::now();
    m.totalWallClockMs = std::chrono::duration<double, std::milli>(batchEnd - batchStart).count();
    m.throughputTasksPerSec = (n * 1000.0) / (m.totalWallClockMs > 0 ? m.totalWallClockMs : 1.0);
    m.speedupVsHostAlone = (referenceHostThroughput > 0.0)
                               ? (m.throughputTasksPerSec / referenceHostThroughput)
                               : 1.0;

    m.p50LatencyMs = computePercentile(latencies, 0.50);
    m.p90LatencyMs = computePercentile(latencies, 0.90);
    m.p95LatencyMs = computePercentile(latencies, 0.95);
    m.p99LatencyMs = computePercentile(latencies, 0.99);
    m.maxLatencyMs = latencies.empty() ? 0.0 : *std::max_element(latencies.begin(), latencies.end());

    // Calculate MAE against reference if available
    if (referenceResults.size() == n) {
        double totalMAE = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            totalMAE += FrameCompositor::computeMAE(referenceResults[i].output, results[i].output);
        }
        m.maeVsReference = totalMAE / n;
    }

    return m;
}

BatchRunMetrics AdvancedBatchScheduler::runDynamicWorkStealing(
    const std::vector<protocol::TaskRequest>& tasks,
    LocalExecFn localExec,
    RemoteExecFn remoteExec,
    bool remoteAvailable,
    double referenceHostThroughput,
    const std::vector<protocol::TaskResult>& referenceResults) {

    uint32_t n = static_cast<uint32_t>(tasks.size());
    BatchRunMetrics m;
    m.modeName = "Dynamic Work-Stealing (Adaptive)";
    m.totalTasks = n;
    m.taskDetails.resize(n);

    std::vector<protocol::TaskResult> results(n);
    std::vector<double> latencies(n, 0.0);

    std::atomic<uint32_t> nextIndex{0};
    std::atomic<uint32_t> hostProcessed{0};
    std::atomic<uint32_t> workerProcessed{0};
    std::atomic<uint32_t> failures{0};

    auto batchStart = std::chrono::steady_clock::now();

    auto hostWorkerLoop = [&]() {
        while (true) {
            uint32_t idx = nextIndex.fetch_add(1);
            if (idx >= n) break;

            auto t0 = std::chrono::steady_clock::now();
            double waitMs = std::chrono::duration<double, std::milli>(t0 - batchStart).count();
            results[idx] = localExec(tasks[idx]);
            auto t1 = std::chrono::steady_clock::now();
            double durMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

            latencies[idx] = durMs;
            m.taskDetails[idx].taskId = tasks[idx].taskId;
            m.taskDetails[idx].executedOn = "Host (RTX 3050)";
            m.taskDetails[idx].queueWaitMs = waitMs;
            m.taskDetails[idx].gpuExecutionMs = static_cast<double>(results[idx].gpuUs) / 1000.0;
            m.taskDetails[idx].totalTurnaroundMs = durMs;
            m.taskDetails[idx].success = (results[idx].status == protocol::TaskStatus::Ok);
            if (!m.taskDetails[idx].success) failures.fetch_add(1);
            hostProcessed.fetch_add(1);
        }
    };

    auto remoteWorkerLoop = [&]() {
        while (true) {
            uint32_t idx = nextIndex.fetch_add(1);
            if (idx >= n) break;

            auto t0 = std::chrono::steady_clock::now();
            double waitMs = std::chrono::duration<double, std::milli>(t0 - batchStart).count();
            if (remoteAvailable && remoteExec) {
                results[idx] = remoteExec(tasks[idx]);
            } else {
                results[idx] = localExec(tasks[idx]);
            }
            auto t1 = std::chrono::steady_clock::now();
            double durMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

            latencies[idx] = durMs;
            m.taskDetails[idx].taskId = tasks[idx].taskId;
            m.taskDetails[idx].executedOn = remoteAvailable ? "Worker (GTX 1650 Ti)" : "Host (Fallback)";
            m.taskDetails[idx].queueWaitMs = waitMs;
            m.taskDetails[idx].gpuExecutionMs = static_cast<double>(results[idx].gpuUs) / 1000.0;
            m.taskDetails[idx].totalTurnaroundMs = durMs;
            m.taskDetails[idx].success = (results[idx].status == protocol::TaskStatus::Ok);
            if (!m.taskDetails[idx].success) failures.fetch_add(1);
            workerProcessed.fetch_add(1);
        }
    };

    if (!remoteAvailable) {
        hostWorkerLoop();
    } else {
        std::thread thHost(hostWorkerLoop);
        std::thread thRemote(remoteWorkerLoop);
        thHost.join();
        thRemote.join();
    }

    auto batchEnd = std::chrono::steady_clock::now();
    m.totalWallClockMs = std::chrono::duration<double, std::milli>(batchEnd - batchStart).count();
    m.throughputTasksPerSec = (n * 1000.0) / (m.totalWallClockMs > 0 ? m.totalWallClockMs : 1.0);
    m.hostTasksExecuted = hostProcessed.load();
    m.workerTasksExecuted = workerProcessed.load();
    m.failedTasks = failures.load();
    m.speedupVsHostAlone = (referenceHostThroughput > 0.0)
                               ? (m.throughputTasksPerSec / referenceHostThroughput)
                               : 1.0;

    m.p50LatencyMs = computePercentile(latencies, 0.50);
    m.p90LatencyMs = computePercentile(latencies, 0.90);
    m.p95LatencyMs = computePercentile(latencies, 0.95);
    m.p99LatencyMs = computePercentile(latencies, 0.99);
    m.maxLatencyMs = latencies.empty() ? 0.0 : *std::max_element(latencies.begin(), latencies.end());

    // Mean queue wait time
    double sumWait = 0.0;
    for (const auto& td : m.taskDetails) sumWait += td.queueWaitMs;
    m.meanQueueWaitMs = sumWait / n;

    if (referenceResults.size() == n) {
        double totalMAE = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            totalMAE += FrameCompositor::computeMAE(referenceResults[i].output, results[i].output);
        }
        m.maeVsReference = totalMAE / n;
    }

    return m;
}

std::vector<SweepPointResult> AdvancedBatchScheduler::runAllocationSweep(
    const std::vector<protocol::TaskRequest>& tasks,
    LocalExecFn localExec,
    RemoteExecFn remoteExec,
    bool remoteAvailable,
    uint32_t step) {

    uint32_t n = static_cast<uint32_t>(tasks.size());
    std::vector<SweepPointResult> sweep;

    // Establish Host Alone baseline first for speedup reference
    auto hostBase = runHostOnly(tasks, localExec);
    double baseThroughput = hostBase.throughputTasksPerSec;

    for (uint32_t hostCount = 0; hostCount <= n; hostCount += step) {
        uint32_t workerCount = n - hostCount;
        auto splitMetrics = runStaticSplit(tasks, hostCount, localExec, remoteExec, remoteAvailable, baseThroughput);

        SweepPointResult pt;
        pt.hostTaskCount = hostCount;
        pt.workerTaskCount = workerCount;
        pt.totalWallClockMs = splitMetrics.totalWallClockMs;
        pt.throughputTasksPerSec = splitMetrics.throughputTasksPerSec;
        pt.speedupVsHostAlone = pt.throughputTasksPerSec / (baseThroughput > 0 ? baseThroughput : 1.0);

        sweep.push_back(pt);
    }

    // Ensure n/0 is explicitly tested if not aligned with step
    if (n % step != 0 && (sweep.empty() || sweep.back().hostTaskCount != n)) {
        auto splitMetrics = runStaticSplit(tasks, n, localExec, remoteExec, remoteAvailable, baseThroughput);
        SweepPointResult pt;
        pt.hostTaskCount = n;
        pt.workerTaskCount = 0;
        pt.totalWallClockMs = splitMetrics.totalWallClockMs;
        pt.throughputTasksPerSec = splitMetrics.throughputTasksPerSec;
        pt.speedupVsHostAlone = pt.throughputTasksPerSec / (baseThroughput > 0 ? baseThroughput : 1.0);
        sweep.push_back(pt);
    }

    return sweep;
}

void AdvancedBatchScheduler::printSweepTable(const std::vector<SweepPointResult>& sweep) {
    std::cout << "\n  ┌───────────────────────┬────────────┬──────────────────┬──────────────┬──────────────┐\n";
    std::cout << "  │ Allocation Split      │ Batch Time │ Throughput       │ vs Host (Ref)│ Load Balance │\n";
    std::cout << "  ├───────────────────────┼────────────┼──────────────────┼──────────────┼──────────────┤\n";
    for (const auto& pt : sweep) {
        std::string splitLabel = std::format("{:>2} Host / {:>2} Worker", pt.hostTaskCount, pt.workerTaskCount);
        std::cout << std::format("  │ {:<21} │ {:>8.2f} ms│ {:>8.1f} tasks/s │ {:>10.2f}x  │ {:>5.1f}% Host │\n",
                                 splitLabel, pt.totalWallClockMs, pt.throughputTasksPerSec, pt.speedupVsHostAlone,
                                 (pt.hostTaskCount + pt.workerTaskCount > 0)
                                     ? (100.0 * pt.hostTaskCount) / (pt.hostTaskCount + pt.workerTaskCount)
                                     : 0.0);
    }
    std::cout << "  └───────────────────────┴────────────┴──────────────────┴──────────────┴──────────────┘\n";
}

void AdvancedBatchScheduler::printMetricsSummary(const BatchRunMetrics& m) {
    std::cout << std::format("  │ {:<28} │ {:>8.2f} ms │ {:>8.1f} t/s │ {:>6.2f}x │ {:>7.2f} ms │ {:>7.2f} ms │ {:>7.2f} ms │\n",
                             m.modeName.substr(0, 28), m.totalWallClockMs, m.throughputTasksPerSec,
                             m.speedupVsHostAlone, m.p50LatencyMs, m.p95LatencyMs, m.maxLatencyMs);
}

} // namespace dgpu::host
