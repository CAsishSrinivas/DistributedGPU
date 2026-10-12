// task_runtime/task_runtime.hpp
// Job submission, state machine, asynchronous queue and scheduler (M5).
#pragma once

#include "protocol/message.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>

namespace dgpu::task_runtime {

using TaskId = uint64_t;

// ─── Job state ───────────────────────────────────────────────────────────────

enum class JobState {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled,
    TimedOut,
};

// ─── Execution site ───────────────────────────────────────────────────────────

enum class ExecutionSite { Local, Remote };

// ─── Job handle returned to callers ──────────────────────────────────────────

struct JobHandle {
    TaskId                          id{};
    std::future<protocol::TaskResult> future;
    ExecutionSite                   site{ExecutionSite::Local};
};

// ─── Scheduler hints / metrics ───────────────────────────────────────────────

struct WorkloadMetrics {
    uint64_t estimatedLocalUs{};    // Estimated local GPU execution (us)
    uint64_t estimatedRemoteUs{};   // Estimated remote GPU execution (us)
    uint64_t measuredNetworkUs{};   // Last measured round-trip (us)
    uint64_t payloadBytes{};
};

// ─── Scheduler decision ───────────────────────────────────────────────────────
//   if (estimatedRemoteUs < estimatedLocalUs) → Remote
//   else                                       → Local
// See M5 for adaptive extension.

[[nodiscard]] ExecutionSite scheduleDecision(const WorkloadMetrics& m) noexcept;

// ─── Local executor callback (pluggable for testing) ─────────────────────────

using LocalExecutor  = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;
using RemoteExecutor = std::function<protocol::TaskResult(const protocol::TaskRequest&)>;

// ─── TaskQueue ───────────────────────────────────────────────────────────────
// Thread-safe FIFO queue that dispatches jobs to local or remote executors.

class TaskQueue {
public:
    explicit TaskQueue(uint32_t maxDepth = 64);
    ~TaskQueue();

    TaskQueue(const TaskQueue&)            = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // Set executors before calling start()
    void setLocalExecutor(LocalExecutor exec);
    void setRemoteExecutor(RemoteExecutor exec);

    // Number of worker threads (default: 2)
    void start(uint32_t workerThreads = 2);
    void stop();

    // Submit a job; returns a future.  May block if the queue is full.
    [[nodiscard]] JobHandle submit(protocol::TaskRequest req,
                                   ExecutionSite site = ExecutionSite::Local);

    // Async version; never blocks — returns nullopt if queue is full
    [[nodiscard]] std::optional<JobHandle>
    trySubmit(protocol::TaskRequest req, ExecutionSite site = ExecutionSite::Local);

    // Cancel a pending job (no-op if already running or done)
    void cancel(TaskId id);

    // Counters for the scheduler
    [[nodiscard]] uint32_t pendingCount()   const noexcept;
    [[nodiscard]] uint32_t runningCount()   const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ─── CircuitBreaker ───────────────────────────────────────────────────────────
// Stops remote submissions when the worker repeatedly misses deadlines or
// disconnects.  Automatically resets after a cool-down period.

class CircuitBreaker {
public:
    struct Config {
        uint32_t  failureThreshold   = 3;      // Consecutive failures to open
        uint32_t  halfOpenTestCount  = 1;      // Probes in half-open state
        std::chrono::seconds coolDown{30};
    };

    explicit CircuitBreaker(Config cfg = {});
    ~CircuitBreaker();

    CircuitBreaker(CircuitBreaker&&) noexcept;
    CircuitBreaker& operator=(CircuitBreaker&&) noexcept;
    CircuitBreaker(const CircuitBreaker&)            = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;

    // Call after each remote attempt
    void recordSuccess();
    void recordFailure();

    // Returns false when the circuit is open (remote is unavailable)
    [[nodiscard]] bool allowRequest() const noexcept;

    enum class State { Closed, Open, HalfOpen };
    [[nodiscard]] State state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dgpu::task_runtime
