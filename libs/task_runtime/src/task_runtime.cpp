// task_runtime/task_runtime.cpp
// Async job queue, scheduler, and circuit breaker implementation.

#include "task_runtime/task_runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dgpu::task_runtime {

// ─── Scheduler decision ───────────────────────────────────────────────────────

ExecutionSite scheduleDecision(const WorkloadMetrics& m) noexcept {
    if (m.estimatedRemoteUs == 0) return ExecutionSite::Local;
    // Include estimated network overhead
    uint64_t remoteTotal = m.estimatedRemoteUs + m.measuredNetworkUs;
    return (remoteTotal < m.estimatedLocalUs)
               ? ExecutionSite::Remote
               : ExecutionSite::Local;
}

// ─── TaskQueue::Impl ─────────────────────────────────────────────────────────

struct TaskEntry {
    protocol::TaskRequest       req;
    ExecutionSite               site;
    std::promise<protocol::TaskResult> promise;
    std::atomic_bool            cancelled{false};
};

struct TaskQueue::Impl {
    uint32_t          maxDepth;
    LocalExecutor     localExec;
    RemoteExecutor    remoteExec;

    std::deque<std::shared_ptr<TaskEntry>> queue;
    std::mutex              mu;
    std::condition_variable cv;
    std::atomic_bool        running{false};
    std::atomic_uint32_t    pending{0};
    std::atomic_uint32_t    running_count{0};
    std::vector<std::thread> workers;

    void workerLoop() {
        while (true) {
            std::shared_ptr<TaskEntry> entry;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return !queue.empty() || !running; });
                if (!running && queue.empty()) break;
                if (queue.empty()) continue;
                entry = queue.front();
                queue.pop_front();
                --pending;
            }

            if (entry->cancelled) {
                protocol::TaskResult err;
                err.taskId = entry->req.taskId;
                err.status = protocol::TaskStatus::InternalError;
                entry->promise.set_value(std::move(err));
                continue;
            }

            ++running_count;
            try {
                protocol::TaskResult result;
                if (entry->site == ExecutionSite::Remote && remoteExec) {
                    result = remoteExec(entry->req);
                } else {
                    if (!localExec) {
                        result.taskId = entry->req.taskId;
                        result.status = protocol::TaskStatus::InternalError;
                    } else {
                        result = localExec(entry->req);
                    }
                }
                entry->promise.set_value(std::move(result));
            } catch (...) {
                protocol::TaskResult err;
                err.taskId = entry->req.taskId;
                err.status = protocol::TaskStatus::InternalError;
                entry->promise.set_value(std::move(err));
            }
            --running_count;
        }
    }
};

TaskQueue::TaskQueue(uint32_t maxDepth)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->maxDepth = maxDepth;
}

TaskQueue::~TaskQueue() { stop(); }

void TaskQueue::setLocalExecutor(LocalExecutor exec) {
    m_impl->localExec = std::move(exec);
}

void TaskQueue::setRemoteExecutor(RemoteExecutor exec) {
    m_impl->remoteExec = std::move(exec);
}

void TaskQueue::start(uint32_t workerThreads) {
    m_impl->running = true;
    for (uint32_t i = 0; i < workerThreads; ++i) {
        m_impl->workers.emplace_back([this] { m_impl->workerLoop(); });
    }
}

void TaskQueue::stop() {
    m_impl->running = false;
    m_impl->cv.notify_all();
    for (auto& t : m_impl->workers) {
        if (t.joinable()) t.join();
    }
    m_impl->workers.clear();
}

JobHandle TaskQueue::submit(protocol::TaskRequest req, ExecutionSite site) {
    auto entry = std::make_shared<TaskEntry>();
    entry->req  = std::move(req);
    entry->site = site;

    JobHandle handle;
    handle.id     = entry->req.taskId;
    handle.future = entry->promise.get_future();
    handle.site   = site;

    {
        std::unique_lock<std::mutex> lk(m_impl->mu);
        // If queue is full, block until space is available
        m_impl->cv.wait(lk, [&] {
            return m_impl->pending < m_impl->maxDepth || !m_impl->running;
        });
        m_impl->queue.push_back(entry);
        ++m_impl->pending;
    }
    m_impl->cv.notify_one();
    return handle;
}

std::optional<JobHandle> TaskQueue::trySubmit(protocol::TaskRequest req,
                                               ExecutionSite site) {
    auto entry = std::make_shared<TaskEntry>();
    entry->req  = std::move(req);
    entry->site = site;

    {
        std::unique_lock<std::mutex> lk(m_impl->mu);
        if (m_impl->pending >= m_impl->maxDepth) return std::nullopt;
        m_impl->queue.push_back(entry);
        ++m_impl->pending;
    }
    m_impl->cv.notify_one();

    JobHandle handle;
    handle.id     = entry->req.taskId;
    handle.future = entry->promise.get_future();
    handle.site   = site;
    return handle;
}

void TaskQueue::cancel(TaskId id) {
    std::unique_lock<std::mutex> lk(m_impl->mu);
    for (auto& e : m_impl->queue) {
        if (e->req.taskId == id) {
            e->cancelled = true;
            break;
        }
    }
}

uint32_t TaskQueue::pendingCount() const noexcept {
    return m_impl->pending.load();
}

uint32_t TaskQueue::runningCount() const noexcept {
    return m_impl->running_count.load();
}

// ─── CircuitBreaker::Impl ────────────────────────────────────────────────────

struct CircuitBreaker::Impl {
    Config    cfg;
    State     state{State::Closed};
    uint32_t  consecutiveFailures{0};
    uint32_t  halfOpenTests{0};
    std::chrono::steady_clock::time_point openedAt;
    mutable std::mutex mu;
};

CircuitBreaker::CircuitBreaker(Config cfg)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->cfg = cfg;
}

CircuitBreaker::~CircuitBreaker() = default;
CircuitBreaker::CircuitBreaker(CircuitBreaker&&) noexcept = default;
CircuitBreaker& CircuitBreaker::operator=(CircuitBreaker&&) noexcept = default;

void CircuitBreaker::recordSuccess() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->consecutiveFailures = 0;
    m_impl->halfOpenTests = 0;
    m_impl->state = State::Closed;
}

void CircuitBreaker::recordFailure() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    ++m_impl->consecutiveFailures;
    if (m_impl->consecutiveFailures >= m_impl->cfg.failureThreshold) {
        if (m_impl->state != State::Open) {
            std::cerr << "[circuit] OPEN — too many remote failures\n";
        }
        m_impl->state    = State::Open;
        m_impl->openedAt = std::chrono::steady_clock::now();
    }
}

bool CircuitBreaker::allowRequest() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->state == State::Closed) return true;
    if (m_impl->state == State::Open) {
        auto elapsed = std::chrono::steady_clock::now() - m_impl->openedAt;
        if (elapsed >= m_impl->cfg.coolDown) {
            m_impl->state = State::HalfOpen;
            m_impl->halfOpenTests = 0;
        } else {
            return false;
        }
    }
    // HalfOpen: allow limited probes
    if (m_impl->halfOpenTests < m_impl->cfg.halfOpenTestCount) {
        ++m_impl->halfOpenTests;
        return true;
    }
    return false;
}

CircuitBreaker::State CircuitBreaker::state() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->state;
}

} // namespace dgpu::task_runtime
