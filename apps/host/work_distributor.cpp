// apps/host/work_distributor.cpp
// Dynamic Work-Stealing Load Balancer implementation for Phase 2.2.

#include "work_distributor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace dgpu::host {

static constexpr double EMA_ALPHA = 0.20;

static double emaUpdate(double prev, double sample, double alpha = EMA_ALPHA) noexcept {
    if (prev <= 0.0) return sample;
    return alpha * sample + (1.0 - alpha) * prev;
}

// ─── DynamicWorkDistributor::Impl ────────────────────────────────────────────

struct DynamicWorkDistributor::Impl {
    WorkDistributorConfig config;

    std::optional<protocol::WorkerCapabilities> workerCaps;
    uint64_t linkBandwidthBps{12'500'000}; // 100 Mbps (~12.5 MB/s)
    uint64_t baseRttUs{1000};              // 1.0 ms base ping

    // Observed processing speeds (moving averages in tasks/sec)
    double hostThroughputTps{100.0};
    double workerThroughputTps{80.0};
    double hostEmaDurationMs{10.0};
    double workerEmaDurationMs{12.5};

    mutable std::mutex mu;

    [[nodiscard]] bool isTaskCompatibleWithWorker(const protocol::TaskRequest& req) const noexcept {
        if (!workerCaps.has_value()) return true;
        return workerCaps->supportsTask(req.type, req.width, req.height);
    }

    [[nodiscard]] bool isTinyTask(const protocol::TaskRequest& req) const noexcept {
        size_t rawBytes = req.input.size();
        if (rawBytes == 0 && req.width > 0 && req.height > 0) {
            rawBytes = static_cast<size_t>(req.width) * req.height * 4;
        }

        // Analytical transfer turnaround time (send + receive)
        double transferUs = (static_cast<double>(rawBytes * 2) / static_cast<double>(linkBandwidthBps)) * 1'000'000.0 +
                            static_cast<double>(baseRttUs);

        // Approximate local GPU compute time
        double pixels = static_cast<double>(req.width) * req.height;
        double estComputeUs = (req.type == protocol::TaskType::Blur) ? (pixels / (1024.0 * 1024.0)) * 25'000.0
                                                                     : (pixels / (1024.0 * 1024.0)) * 8'000.0;

        // If transfer alone exceeds local execution, or if compute duration is below the tiny-task threshold
        return (transferUs >= estComputeUs * 0.90) || (estComputeUs < config.minComputeUsForRemote);
    }
};

DynamicWorkDistributor::DynamicWorkDistributor(WorkDistributorConfig config) : m_impl(std::make_unique<Impl>()) {
    m_impl->config = config;
}

DynamicWorkDistributor::~DynamicWorkDistributor() = default;

void DynamicWorkDistributor::setWorkerCapabilities(const protocol::WorkerCapabilities& caps) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->workerCaps = caps;
}

void DynamicWorkDistributor::clearWorkerCapabilities() noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->workerCaps.reset();
}

void DynamicWorkDistributor::setLinkBandwidthBps(uint64_t bps) noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->linkBandwidthBps = (bps > 0) ? bps : 12'500'000;
}

void DynamicWorkDistributor::setBaseRttUs(uint64_t rttUs) noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->baseRttUs = rttUs;
}

const WorkDistributorConfig& DynamicWorkDistributor::config() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->config;
}

void DynamicWorkDistributor::setConfig(const WorkDistributorConfig& config) noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->config = config;
}

// ─── Work-Stealing Batch Execution ───────────────────────────────────────────

struct QueuedItem {
    uint32_t originalIndex{0};
    protocol::TaskRequest request;
};

WorkDistributorMetrics DynamicWorkDistributor::executeBatch(const std::vector<protocol::TaskRequest>& tasks,
                                                            TaskExecFn localExec, TaskExecFn remoteExec,
                                                            bool remoteAvailable,
                                                            std::vector<protocol::TaskResult>* outResults) {
    const uint32_t n = static_cast<uint32_t>(tasks.size());
    if (outResults) {
        outResults->resize(n);
    }

    WorkDistributorMetrics metrics;
    metrics.totalTasks = n;

    if (n == 0) return metrics;

    std::deque<QueuedItem> hostQueue;
    std::deque<QueuedItem> workerQueue;
    std::mutex queueMu;
    std::condition_variable queueCv;

    std::atomic<bool> done{false};
    std::atomic<uint32_t> completedCount{0};
    std::atomic<uint32_t> hostProcessed{0};
    std::atomic<uint32_t> workerProcessed{0};
    std::atomic<uint32_t> stolenByHost{0};
    std::atomic<uint32_t> stolenByWorker{0};
    std::atomic<uint32_t> tinyRetained{0};
    std::atomic<uint32_t> failures{0};

    std::atomic<uint32_t> hostActive{0};
    std::atomic<uint32_t> workerActive{0};

    // 1. Initial Speed-Proportional Dispatch with Bounded Queues
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);

        double totalRate = m_impl->hostThroughputTps + m_impl->workerThroughputTps;
        double hostFraction = (remoteAvailable && totalRate > 0) ? (m_impl->hostThroughputTps / totalRate) : 1.0;

        uint32_t hostTargetCount = static_cast<uint32_t>(std::round(static_cast<double>(n) * hostFraction));
        uint32_t hostAssigned = 0;

        for (uint32_t i = 0; i < n; ++i) {
            QueuedItem item{i, tasks[i]};

            bool isTiny = m_impl->isTinyTask(tasks[i]);
            bool isCompatible = m_impl->isTaskCompatibleWithWorker(tasks[i]);

            if (isTiny) {
                tinyRetained.fetch_add(1);
            }

            // Route to Host if tiny, incompatible, remote offline, or within host target fraction
            if (!remoteAvailable || isTiny || !isCompatible || (hostAssigned < hostTargetCount)) {
                hostQueue.push_back(std::move(item));
                ++hostAssigned;
            } else {
                // Apply backpressure if worker queue is full
                if (workerQueue.size() >= m_impl->config.maxQueueDepth) {
                    hostQueue.push_back(std::move(item));
                    ++hostAssigned;
                } else {
                    workerQueue.push_back(std::move(item));
                }
            }
        }
    }

    auto batchStart = std::chrono::steady_clock::now();

    // 2. Host Execution Loop (with Work Stealing from Worker Queue)
    auto hostWorkerLoop = [&]() {
        while (completedCount.load() < n) {
            QueuedItem item;
            bool hasItem = false;
            bool wasStolen = false;

            {
                std::unique_lock<std::mutex> lk(queueMu);
                if (!hostQueue.empty()) {
                    item = std::move(hostQueue.front());
                    hostQueue.pop_front();
                    hasItem = true;
                } else if (m_impl->config.allowWorkStealing && workerQueue.size() >= m_impl->config.stealThreshold) {
                    // Steal from back of worker queue (LIFO stealing)
                    item = std::move(workerQueue.back());
                    workerQueue.pop_back();
                    hasItem = true;
                    wasStolen = true;
                    stolenByHost.fetch_add(1);
                } else {
                    if (completedCount.load() >= n) break;
                    // Wait for work or completion
                    queueCv.wait_for(lk, std::chrono::milliseconds(2), [&] {
                        return !hostQueue.empty() || completedCount.load() >= n ||
                               (m_impl->config.allowWorkStealing &&
                                workerQueue.size() >= m_impl->config.stealThreshold);
                    });
                    continue;
                }
            }

            if (!hasItem) continue;

            hostActive.fetch_add(1);
            auto t0 = std::chrono::steady_clock::now();
            auto result = localExec(item.request);
            auto t1 = std::chrono::steady_clock::now();
            double durMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            hostActive.fetch_sub(1);

            if (outResults) {
                (*outResults)[item.originalIndex] = result;
            }

            if (result.status != protocol::TaskStatus::Ok) {
                failures.fetch_add(1);
            }

            hostProcessed.fetch_add(1);
            completedCount.fetch_add(1);

            // Update host EMA processing speed
            {
                std::lock_guard<std::mutex> lk(m_impl->mu);
                m_impl->hostEmaDurationMs = emaUpdate(m_impl->hostEmaDurationMs, durMs);
                if (durMs > 0.05) {
                    double instTps = 1000.0 / durMs;
                    m_impl->hostThroughputTps = emaUpdate(m_impl->hostThroughputTps, instTps);
                }
            }

            queueCv.notify_all();
        }
    };

    // 3. Worker Execution Loop (with Work Stealing from Host Queue)
    auto remoteWorkerLoop = [&]() {
        while (completedCount.load() < n) {
            QueuedItem item;
            bool hasItem = false;
            bool wasStolen = false;

            {
                std::unique_lock<std::mutex> lk(queueMu);
                if (!workerQueue.empty()) {
                    item = std::move(workerQueue.front());
                    workerQueue.pop_front();
                    hasItem = true;
                } else if (m_impl->config.allowWorkStealing && hostQueue.size() >= m_impl->config.stealThreshold) {
                    // Check if candidate task at back of hostQueue is eligible to be stolen
                    const auto& candidate = hostQueue.back();
                    bool canSteal =
                        !m_impl->isTinyTask(candidate.request) && m_impl->isTaskCompatibleWithWorker(candidate.request);

                    if (canSteal) {
                        item = std::move(hostQueue.back());
                        hostQueue.pop_back();
                        hasItem = true;
                        wasStolen = true;
                        stolenByWorker.fetch_add(1);
                    }
                }

                if (!hasItem) {
                    if (completedCount.load() >= n) break;
                    queueCv.wait_for(lk, std::chrono::milliseconds(2), [&] {
                        return !workerQueue.empty() || completedCount.load() >= n ||
                               (m_impl->config.allowWorkStealing && hostQueue.size() >= m_impl->config.stealThreshold);
                    });
                    continue;
                }
            }

            if (!hasItem) continue;

            workerActive.fetch_add(1);
            auto t0 = std::chrono::steady_clock::now();
            protocol::TaskResult result;
            if (remoteAvailable && remoteExec) {
                result = remoteExec(item.request);
            } else {
                result = localExec(item.request);
            }
            auto t1 = std::chrono::steady_clock::now();
            double durMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            workerActive.fetch_sub(1);

            if (outResults) {
                (*outResults)[item.originalIndex] = result;
            }

            if (result.status != protocol::TaskStatus::Ok) {
                failures.fetch_add(1);
            }

            workerProcessed.fetch_add(1);
            completedCount.fetch_add(1);

            // Update worker EMA processing speed
            {
                std::lock_guard<std::mutex> lk(m_impl->mu);
                m_impl->workerEmaDurationMs = emaUpdate(m_impl->workerEmaDurationMs, durMs);
                if (durMs > 0.05) {
                    double instTps = 1000.0 / durMs;
                    m_impl->workerThroughputTps = emaUpdate(m_impl->workerThroughputTps, instTps);
                }
            }

            queueCv.notify_all();
        }
    };

    if (!remoteAvailable) {
        hostWorkerLoop();
    } else {
        std::thread thHost(hostWorkerLoop);
        std::thread thWorker(remoteWorkerLoop);
        thHost.join();
        thWorker.join();
    }

    auto batchEnd = std::chrono::steady_clock::now();
    metrics.totalWallClockMs = std::chrono::duration<double, std::milli>(batchEnd - batchStart).count();
    metrics.throughputTasksPerSec = (n * 1000.0) / (metrics.totalWallClockMs > 0 ? metrics.totalWallClockMs : 1.0);
    metrics.hostTasksExecuted = hostProcessed.load();
    metrics.workerTasksExecuted = workerProcessed.load();
    metrics.tasksStolenByHost = stolenByHost.load();
    metrics.tasksStolenByWorker = stolenByWorker.load();
    metrics.tinyTasksRetainedLocally = tinyRetained.load();
    metrics.failedTasks = failures.load();

    // Fill runtime stats snapshot
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        metrics.hostStats.workerId = "host-0";
        metrics.hostStats.deviceName = "Host (RTX 3050)";
        metrics.hostStats.completedJobs = metrics.hostTasksExecuted;
        metrics.hostStats.stolenJobs = metrics.tasksStolenByHost;
        metrics.hostStats.emaDurationMs = m_impl->hostEmaDurationMs;
        metrics.hostStats.throughputTasksPerSec = m_impl->hostThroughputTps;
        metrics.hostStats.isAvailable = true;

        metrics.workerStats.workerId = "worker-1";
        metrics.workerStats.deviceName = "Worker (GTX 1650 Ti)";
        metrics.workerStats.completedJobs = metrics.workerTasksExecuted;
        metrics.workerStats.stolenJobs = metrics.tasksStolenByWorker;
        metrics.workerStats.emaDurationMs = m_impl->workerEmaDurationMs;
        metrics.workerStats.throughputTasksPerSec = m_impl->workerThroughputTps;
        metrics.workerStats.isAvailable = remoteAvailable;
    }

    return metrics;
}

} // namespace dgpu::host
