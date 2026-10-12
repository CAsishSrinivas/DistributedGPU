// telemetry/telemetry.hpp
// Timing, throughput, error recording and CSV export (used by all modules).
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dgpu::telemetry {

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ─── FrameRecord — one benchmark sample ──────────────────────────────────────

struct FrameRecord {
    uint64_t    frameIndex{};
    std::string label;          // e.g. "local_512x512" or "remote_async_1080p"

    // All times in microseconds
    uint64_t cpuSubmitUs{};
    uint64_t gpuExecUs{};
    uint64_t readbackUs{};
    uint64_t uploadUs{};
    uint64_t networkSendUs{};
    uint64_t networkReturnUs{};
    uint64_t totalUs{};

    bool     isRemote{false};
    uint32_t width{};
    uint32_t height{};
};

// ─── BenchmarkReport ─────────────────────────────────────────────────────────

struct BenchmarkReport {
    std::string label;
    uint64_t    sampleCount{};

    // Statistics over totalUs
    uint64_t    minUs{};
    uint64_t    maxUs{};
    double      meanUs{};
    double      p50Us{};
    double      p95Us{};
    double      p99Us{};
    double      stddevUs{};

    double      avgFps{};
};

// ─── Telemetry singleton ─────────────────────────────────────────────────────

class Telemetry {
public:
    static Telemetry& instance();

    // Record a completed frame
    void record(FrameRecord rec);

    // Generate a summary report for all records with the given label prefix
    [[nodiscard]] BenchmarkReport summarise(const std::string& labelPrefix) const;

    // Export all records to a CSV file
    [[nodiscard]] bool exportCsv(const std::filesystem::path& path) const;

    // Clear all records (e.g. after warm-up runs)
    void clear();

    [[nodiscard]] std::size_t count() const noexcept;

private:
    Telemetry();
    ~Telemetry();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ─── ScopedTimer — RAII helper for CPU-side timing ───────────────────────────

class ScopedTimer {
public:
    explicit ScopedTimer(uint64_t& outMicroseconds)
        : m_out(outMicroseconds), m_start(Clock::now()) {}

    ~ScopedTimer() {
        using namespace std::chrono;
        m_out = duration_cast<microseconds>(Clock::now() - m_start).count();
    }

private:
    uint64_t&  m_out;
    TimePoint  m_start;
};

} // namespace dgpu::telemetry
