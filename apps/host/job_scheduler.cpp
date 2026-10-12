// apps/host/job_scheduler.cpp
// Adaptive scheduler: EMA-based local vs remote decision + circuit breaker.

#include "job_scheduler.hpp"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <optional>

namespace dgpu::host {

static constexpr double EMA_ALPHA = 0.1;

static double emaUpdate(double prev, double newSample) noexcept {
    if (prev == 0.0) return newSample; // cold-start: use first sample directly
    return EMA_ALPHA * newSample + (1.0 - EMA_ALPHA) * prev;
}

// ─── JobScheduler::Impl ──────────────────────────────────────────────────────

#include <unordered_map>

struct JobScheduler::Impl {
    task_runtime::TaskQueue                   queue;
    task_runtime::CircuitBreaker              breaker;
    SchedulerMetrics                          metrics;
    std::unordered_map<uint64_t, ResolutionMetrics> resMap;
    uint64_t                                  linkBandwidthBps{12'500'000}; // 100 Mbps (~12.5 MB/s)
    uint64_t                                  baseRttUs{1000};              // 1.0 ms base network latency
    mutable std::mutex                        mu;

    static uint64_t makeResKey(uint32_t w, uint32_t h) noexcept {
        return (static_cast<uint64_t>(w) << 32) | static_cast<uint64_t>(h);
    }
};

JobScheduler::JobScheduler() : m_impl(std::make_unique<Impl>()) {}
JobScheduler::~JobScheduler() { stop(); }

void JobScheduler::setLocalExecutor(task_runtime::LocalExecutor exec) {
    m_impl->queue.setLocalExecutor(std::move(exec));
}

void JobScheduler::setRemoteExecutor(task_runtime::RemoteExecutor exec) {
    m_impl->queue.setRemoteExecutor(std::move(exec));
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
}

uint64_t JobScheduler::linkBandwidthBps() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->linkBandwidthBps;
}

void JobScheduler::setBaseRttUs(uint64_t rttUs) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->baseRttUs = rttUs;
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

    double transferUs = (static_cast<double>(payloadBytes * 2) /
                         static_cast<double>(m_impl->linkBandwidthBps)) * 1'000'000.0
                        + static_cast<double>(m_impl->baseRttUs);

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

task_runtime::ExecutionSite
JobScheduler::decide(const protocol::TaskRequest& req) const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);

    // Look up resolution-specific metrics
    uint64_t key = Impl::makeResKey(req.width, req.height);
    auto it = m_impl->resMap.find(key);
    if (it == m_impl->resMap.end()) {
        // Cold start for this resolution: probe local first to establish ground truth
        return task_runtime::ExecutionSite::Local;
    }

    const auto& rm = it->second;
    if (rm.localSamples == 0) {
        return task_runtime::ExecutionSite::Local;
    }

    // ── Analytical Link Constraint Check (Priority 1) ────────────────────────
    // Transmitting input payload to worker and returning output: 2x payload bytes
    uint64_t payloadBytes = req.input.size();
    if (payloadBytes == 0) {
        payloadBytes = static_cast<uint64_t>(req.width) * req.height * 4;
    }

    double estTransferUs = (static_cast<double>(payloadBytes * 2) /
                            static_cast<double>(m_impl->linkBandwidthBps)) * 1'000'000.0
                           + static_cast<double>(m_impl->baseRttUs);

    // If network transfer ALONE exceeds local GPU execution time,
    // remote offloading is mathematically impossible to be faster on this link.
    // Avoid probing remote or stalling the pipeline!
    if (estTransferUs >= rm.emaLocalTotalUs * 0.95) {
        return task_runtime::ExecutionSite::Local;
    }

    if (rm.remoteSamples == 0) {
        // Network transfer is small enough that remote execution is physically plausible:
        // probe remote once to establish ground truth for this resolution
        return task_runtime::ExecutionSite::Remote;
    }

    // Both local and remote have measured samples:
    // Compare end-to-end estimated turnaround times
    double estimatedRemote = rm.emaRemoteGpuUs + rm.emaNetworkRttUs;
    double estimatedLocal  = rm.emaLocalTotalUs;

    return (estimatedRemote < estimatedLocal)
               ? task_runtime::ExecutionSite::Remote
               : task_runtime::ExecutionSite::Local;
}

task_runtime::JobHandle
JobScheduler::submit(protocol::TaskRequest req,
                     std::optional<task_runtime::ExecutionSite> forceSite) {
    task_runtime::ExecutionSite site =
        forceSite.has_value() ? *forceSite : decide(req);

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
    recordRemoteSuccess(0, 0, remoteGpuUs, networkUs);
}

void JobScheduler::recordRemoteSuccess(uint32_t width, uint32_t height,
                                       uint64_t remoteGpuUs, uint64_t networkUs) {
    m_impl->breaker.recordSuccess();
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->metrics.emaRemoteUs  = emaUpdate(m_impl->metrics.emaRemoteUs,
                                              static_cast<double>(remoteGpuUs));
    m_impl->metrics.emaNetworkUs = emaUpdate(m_impl->metrics.emaNetworkUs,
                                              static_cast<double>(networkUs));
    ++m_impl->metrics.remoteSamples;

    if (width > 0 && height > 0) {
        uint64_t key = Impl::makeResKey(width, height);
        auto& rm = m_impl->resMap[key];
        rm.emaRemoteGpuUs  = emaUpdate(rm.emaRemoteGpuUs, static_cast<double>(remoteGpuUs));
        rm.emaNetworkRttUs = emaUpdate(rm.emaNetworkRttUs, static_cast<double>(networkUs));
        ++rm.remoteSamples;

        if (networkUs > 500) {
            double totalBytes = static_cast<double>(static_cast<uint64_t>(width) * height * 4 * 2);
            double measuredBps = (totalBytes / static_cast<double>(networkUs)) * 1'000'000.0;
            m_impl->linkBandwidthBps = static_cast<uint64_t>(
                emaUpdate(static_cast<double>(m_impl->linkBandwidthBps), measuredBps));
        }
    }
}

void JobScheduler::recordRemoteFailure() {
    m_impl->breaker.recordFailure();
}

void JobScheduler::recordLocalSuccess(uint64_t localTotalUs, uint64_t payloadBytes) {
    recordLocalSuccess(0, 0, localTotalUs, payloadBytes);
}

void JobScheduler::recordLocalSuccess(uint32_t width, uint32_t height,
                                      uint64_t localTotalUs, uint64_t payloadBytes) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->metrics.emaLocalUs   = emaUpdate(m_impl->metrics.emaLocalUs,
                                              static_cast<double>(localTotalUs));
    m_impl->metrics.emaPayloadKb = emaUpdate(m_impl->metrics.emaPayloadKb,
                                              static_cast<double>(payloadBytes) / 1024.0);
    ++m_impl->metrics.localSamples;

    if (width > 0 && height > 0) {
        uint64_t key = Impl::makeResKey(width, height);
        auto& rm = m_impl->resMap[key];
        rm.emaLocalTotalUs = emaUpdate(rm.emaLocalTotalUs, static_cast<double>(localTotalUs));
        ++rm.localSamples;
    }
}

const SchedulerMetrics& JobScheduler::metrics() const noexcept {
    return m_impl->metrics;
}

task_runtime::CircuitBreaker::State JobScheduler::circuitState() const noexcept {
    return m_impl->breaker.state();
}

} // namespace dgpu::host
