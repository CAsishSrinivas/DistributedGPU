// apps/host/job_scheduler.cpp
// Workload-aware scheduler: Multi-strategy execution cost model + circuit breaker.

#include "job_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace dgpu::host {

static constexpr double EMA_ALPHA = 0.1;

static double emaUpdate(double prev, double newSample) noexcept {
    if (prev == 0.0) return newSample; // cold-start: use first sample directly
    return EMA_ALPHA * newSample + (1.0 - EMA_ALPHA) * prev;
}

// ─── JobScheduler::Impl ──────────────────────────────────────────────────────

struct JobScheduler::Impl {
    task_runtime::TaskQueue queue;
    task_runtime::CircuitBreaker breaker;
    SchedulerMetrics metrics;
    WorkloadCostModel costModel;
    std::unordered_map<uint64_t, ResolutionMetrics> resMap;
    std::optional<protocol::WorkerCapabilities> workerCaps;
    uint64_t linkBandwidthBps{12'500'000}; // 100 Mbps (~12.5 MB/s)
    uint64_t baseRttUs{1000};              // 1.0 ms base network latency
    mutable std::mutex mu;

    static uint64_t makeResKey(uint32_t w, uint32_t h) noexcept {
        return (static_cast<uint64_t>(w) << 32) | static_cast<uint64_t>(h);
    }
};

JobScheduler::JobScheduler() : m_impl(std::make_unique<Impl>()) {}
JobScheduler::~JobScheduler() {
    stop();
}

void JobScheduler::setLocalExecutor(task_runtime::LocalExecutor exec) {
    auto wrapped = [this, userExec = std::move(exec)](const protocol::TaskRequest& req) {
        auto start = std::chrono::steady_clock::now();
        auto result = userExec(req);
        auto end = std::chrono::steady_clock::now();
        uint64_t totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        uint64_t gpuUs = (result.gpuUs > 0) ? result.gpuUs : totalUs;
        this->recordLocalSuccess(req.type, req.width, req.height, totalUs, req.input.size());
        m_impl->costModel.recordLocalSuccess(req.type, req.width, req.height, gpuUs, totalUs);
        return result;
    };
    m_impl->queue.setLocalExecutor(std::move(wrapped));
}

void JobScheduler::setRemoteExecutor(task_runtime::RemoteExecutor exec) {
    auto wrapped = [this, userExec = std::move(exec)](const protocol::TaskRequest& req) {
        auto start = std::chrono::steady_clock::now();
        auto result = userExec(req);
        auto end = std::chrono::steady_clock::now();
        uint64_t totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        if (result.status == protocol::TaskStatus::Ok) {
            uint64_t workerExecUs = (result.totalUs > 0) ? result.totalUs : result.gpuUs;
            uint64_t codecUs = result.compressUs + result.decompressUs + result.serializeUs + result.deserializeUs;
            uint64_t netUs = (result.pureNetworkRttUs > 0)
                                 ? result.pureNetworkRttUs
                                 : ((totalUs > (workerExecUs + codecUs)) ? (totalUs - (workerExecUs + codecUs)) : 1000);

            size_t sendWire = (result.wireSendBytes > 0) ? result.wireSendBytes : req.input.size();
            size_t recvWire = (result.wireRecvBytes > 0) ? result.wireRecvBytes : result.output.size();
            size_t uncompSend = (result.uncompressedSendBytes > 0) ? result.uncompressedSendBytes : req.input.size();

            this->recordRemoteSuccess(req.type, req.width, req.height, result.gpuUs, netUs,
                                      uncompSend, sendWire, result.compressUs, result.decompressUs,
                                      sendWire, recvWire, totalUs);
        } else {
            this->recordRemoteFailure();
        }
        return result;
    };
    m_impl->queue.setRemoteExecutor(std::move(wrapped));
}

void JobScheduler::setWorkerCapabilities(const protocol::WorkerCapabilities& caps) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->workerCaps = caps;
    m_impl->costModel.setWorkerCapabilities(caps);
}

void JobScheduler::start(uint32_t workerThreads) {
    m_impl->queue.start(workerThreads);
}

void JobScheduler::stop() {
    m_impl->queue.stop();
}

void JobScheduler::setLinkBandwidthBps(uint64_t bytesPerSec) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->linkBandwidthBps = (bytesPerSec > 0) ? bytesPerSec : 12'500'000;
    m_impl->costModel.setLinkBandwidthBps(m_impl->linkBandwidthBps);
}

uint64_t JobScheduler::linkBandwidthBps() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->linkBandwidthBps;
}

void JobScheduler::setBaseRttUs(uint64_t rttUs) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->baseRttUs = rttUs;
    m_impl->costModel.setBaseRttUs(rttUs);
}

uint64_t JobScheduler::baseRttUs() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->baseRttUs;
}

double JobScheduler::estimateRemoteTurnaroundUs(uint32_t width, uint32_t height, uint64_t payloadBytes) const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (payloadBytes == 0 && width > 0 && height > 0) {
        payloadBytes = static_cast<uint64_t>(width) * height * 4;
    }

    auto eval =
        m_impl->costModel.evaluateStrategies(protocol::TaskType::Blur, width, height, static_cast<size_t>(payloadBytes),
                                             0, 0, m_impl->breaker.allowRequest());
    for (const auto& s : eval.evaluatedStrategies) {
        if (s.strategy == ExecutionStrategy::WorkerOnly && s.isEligible) {
            return s.breakdown.totalTurnaroundUs;
        }
    }

    double transferUs =
        (static_cast<double>(payloadBytes * 2) / static_cast<double>(m_impl->linkBandwidthBps)) * 1'000'000.0 +
        static_cast<double>(m_impl->baseRttUs);

    uint64_t key = Impl::makeResKey(width, height);
    auto it = m_impl->resMap.find(key);
    double estGpuUs = 0.0;
    if (it != m_impl->resMap.end() && it->second.remoteSamples > 0) {
        estGpuUs = it->second.emaRemoteGpuUs;
    } else if (it != m_impl->resMap.end() && it->second.localSamples > 0) {
        estGpuUs = it->second.emaLocalTotalUs * 1.1; // estimate worker GPU ~ 1.1x local
    } else {
        estGpuUs = m_impl->metrics.emaRemoteUs > 0 ? m_impl->metrics.emaRemoteUs : 5000.0;
    }

    return transferUs + estGpuUs;
}

WorkloadCostModel& JobScheduler::costModel() noexcept {
    return m_impl->costModel;
}

const WorkloadCostModel& JobScheduler::costModel() const noexcept {
    return m_impl->costModel;
}

MultiStrategyComparison JobScheduler::evaluateStrategies(const protocol::TaskRequest& req, uint32_t batchSize) const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    bool remoteOk = m_impl->breaker.allowRequest();
    return m_impl->costModel.evaluateStrategies(req, m_impl->queue.pendingCount(), 0, remoteOk, batchSize);
}

task_runtime::ExecutionSite JobScheduler::decide(const protocol::TaskRequest& req) const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);

    bool remoteOk = m_impl->breaker.allowRequest();
    uint32_t pending = m_impl->queue.pendingCount();

    // Workload-aware multi-strategy evaluation
    auto comparison = m_impl->costModel.evaluateStrategies(req, pending, 0, remoteOk);

    // If optimal strategy is WorkerOnly and not falling back to local
    if (comparison.optimalStrategy == ExecutionStrategy::WorkerOnly && !comparison.isLocalFallback) {
        return task_runtime::ExecutionSite::Remote;
    }

    return task_runtime::ExecutionSite::Local;
}

task_runtime::JobHandle JobScheduler::submit(protocol::TaskRequest req,
                                             std::optional<task_runtime::ExecutionSite> forceSite) {
    task_runtime::ExecutionSite site = forceSite.has_value() ? *forceSite : decide(req);

    // Pre-dispatch capability check for remote
    if (site == task_runtime::ExecutionSite::Remote && !forceSite.has_value()) {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->workerCaps.has_value() && !m_impl->workerCaps->supportsTask(req.type, req.width, req.height)) {
            site = task_runtime::ExecutionSite::Local;
            ++m_impl->metrics.fallbackCount;
        }
    }

    // Circuit breaker check for remote
    if (site == task_runtime::ExecutionSite::Remote) {
        if (!m_impl->breaker.allowRequest()) {
            std::cout << "[scheduler] Circuit open — falling back to local\n";
            site = task_runtime::ExecutionSite::Local;
            std::lock_guard<std::mutex> lk(m_impl->mu);
            ++m_impl->metrics.fallbackCount;
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (site == task_runtime::ExecutionSite::Local)
            ++m_impl->metrics.localCount;
        else
            ++m_impl->metrics.remoteCount;
    }

    return m_impl->queue.submit(std::move(req), site);
}

void JobScheduler::recordRemoteSuccess(uint64_t remoteGpuUs, uint64_t networkUs) {
    recordRemoteSuccess(protocol::TaskType::Blur, 0, 0, remoteGpuUs, networkUs);
}

void JobScheduler::recordRemoteSuccess(uint32_t width, uint32_t height, uint64_t remoteGpuUs, uint64_t networkUs) {
    recordRemoteSuccess(protocol::TaskType::Blur, width, height, remoteGpuUs, networkUs);
}

void JobScheduler::recordRemoteSuccess(protocol::TaskType type, uint32_t width, uint32_t height, uint64_t remoteGpuUs,
                                       uint64_t networkUs, size_t uncompressedBytes, size_t compressedBytes,
                                       uint64_t compressUs, uint64_t decompressUs,
                                       size_t wireSendBytes, size_t wireRecvBytes, uint64_t totalTurnaroundUs) {
    m_impl->breaker.recordSuccess();
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->metrics.emaRemoteUs = emaUpdate(m_impl->metrics.emaRemoteUs, static_cast<double>(remoteGpuUs));
    m_impl->metrics.emaNetworkUs = emaUpdate(m_impl->metrics.emaNetworkUs, static_cast<double>(networkUs));
    ++m_impl->metrics.remoteSamples;

    if (width > 0 && height > 0) {
        uint64_t key = Impl::makeResKey(width, height);
        auto& rm = m_impl->resMap[key];
        rm.emaRemoteGpuUs = emaUpdate(rm.emaRemoteGpuUs, static_cast<double>(remoteGpuUs));
        rm.emaNetworkRttUs = emaUpdate(rm.emaNetworkRttUs, static_cast<double>(networkUs));
        ++rm.remoteSamples;

        // Use actual measured wire bytes rather than an uncompressed full-frame assumption
        size_t totalWire = wireSendBytes + wireRecvBytes;
        if (totalWire == 0) {
            totalWire = (compressedBytes > 0) ? (compressedBytes * 2) : (static_cast<uint64_t>(width) * height * 4 * 2);
        }

        if (networkUs > 50 && totalWire > 0) {
            double measuredBps = (static_cast<double>(totalWire) / static_cast<double>(networkUs)) * 1'000'000.0;
            m_impl->linkBandwidthBps =
                static_cast<uint64_t>(emaUpdate(static_cast<double>(m_impl->linkBandwidthBps), measuredBps));
            m_impl->costModel.setLinkBandwidthBps(m_impl->linkBandwidthBps);
        }

        uint64_t turnaround = (totalTurnaroundUs > 0) ? totalTurnaroundUs : (remoteGpuUs + networkUs);
        m_impl->costModel.recordRemoteSuccess(type, width, height, remoteGpuUs, networkUs, turnaround,
                                              uncompressedBytes, compressedBytes, compressUs, decompressUs);
    }
}

void JobScheduler::recordRemoteFailure() {
    m_impl->breaker.recordFailure();
}

void JobScheduler::recordLocalSuccess(uint64_t localTotalUs, uint64_t payloadBytes) {
    recordLocalSuccess(protocol::TaskType::Blur, 0, 0, localTotalUs, payloadBytes);
}

void JobScheduler::recordLocalSuccess(uint32_t width, uint32_t height, uint64_t localTotalUs, uint64_t payloadBytes) {
    recordLocalSuccess(protocol::TaskType::Blur, width, height, localTotalUs, payloadBytes);
}

void JobScheduler::recordLocalSuccess(protocol::TaskType type, uint32_t width, uint32_t height, uint64_t localTotalUs,
                                      uint64_t payloadBytes) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->metrics.emaLocalUs = emaUpdate(m_impl->metrics.emaLocalUs, static_cast<double>(localTotalUs));
    m_impl->metrics.emaPayloadKb = emaUpdate(m_impl->metrics.emaPayloadKb, static_cast<double>(payloadBytes) / 1024.0);
    ++m_impl->metrics.localSamples;

    if (width > 0 && height > 0) {
        uint64_t key = Impl::makeResKey(width, height);
        auto& rm = m_impl->resMap[key];
        rm.emaLocalTotalUs = emaUpdate(rm.emaLocalTotalUs, static_cast<double>(localTotalUs));
        ++rm.localSamples;

        m_impl->costModel.recordLocalSuccess(type, width, height, localTotalUs, localTotalUs);
    }
}

SchedulerMetrics JobScheduler::metrics() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->metrics;
}

task_runtime::CircuitBreaker::State JobScheduler::circuitState() const noexcept {
    return m_impl->breaker.state();
}

} // namespace dgpu::host
