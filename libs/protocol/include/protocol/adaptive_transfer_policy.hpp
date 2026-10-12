// protocol/adaptive_transfer_policy.hpp
// Adaptive compression, throughput estimation, and transfer policy (Phase 2.6).
#pragma once

#include "protocol/lz4.hpp"
#include "protocol/message.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dgpu::protocol {

enum class CodecDecision : uint8_t {
    Compress,
    BypassRaw,
};

struct TransferEvaluation {
    CodecDecision decision{CodecDecision::Compress};
    float         estimatedRatio{1.0f};
    double        predictedRawUs{0.0};
    double        predictedCompressedUs{0.0};
    double        predictedSavingsUs{0.0};
    bool          isProfitable{true};
    std::string   rationale;
};

struct CodecThroughputStats {
    double   compressSpeedBytesPerSec{500'000'000.0};     // Default ~500 MB/s
    double   decompressSpeedBytesPerSec{1'500'000'000.0}; // Default ~1.5 GB/s
    float    rollingCompressionRatio{1.0f};
    uint64_t totalBytesEvaluated{0};
    uint64_t totalBytesCompressed{0};
    uint64_t totalBytesBypassed{0};
    uint64_t compressDecisions{0};
    uint64_t bypassDecisions{0};
    uint64_t totalCompressionUs{0};
    uint64_t totalDecompressionUs{0};
};

class AdaptiveTransferPolicy {
public:
    AdaptiveTransferPolicy() = default;
    ~AdaptiveTransferPolicy() = default;

    AdaptiveTransferPolicy(const AdaptiveTransferPolicy& other) {
        std::lock_guard<std::mutex> lk(other.m_mu);
        m_stats = other.m_stats;
        m_force = other.m_force;
        m_minPayloadBytes = other.m_minPayloadBytes;
    }

    AdaptiveTransferPolicy& operator=(const AdaptiveTransferPolicy& other) {
        if (this != &other) {
            std::scoped_lock lk(m_mu, other.m_mu);
            m_stats = other.m_stats;
            m_force = other.m_force;
            m_minPayloadBytes = other.m_minPayloadBytes;
        }
        return *this;
    }

    AdaptiveTransferPolicy(AdaptiveTransferPolicy&& other) noexcept {
        std::lock_guard<std::mutex> lk(other.m_mu);
        m_stats = other.m_stats;
        m_force = other.m_force;
        m_minPayloadBytes = other.m_minPayloadBytes;
    }

    AdaptiveTransferPolicy& operator=(AdaptiveTransferPolicy&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lk(m_mu, other.m_mu);
            m_stats = other.m_stats;
            m_force = other.m_force;
            m_minPayloadBytes = other.m_minPayloadBytes;
        }
        return *this;
    }

    // Fast compressibility estimator via multi-point sampling (probe size 1KB - 4KB)
    [[nodiscard]] static float estimateCompressibility(std::span<const std::byte> data) noexcept;

    // Evaluates whether compression will yield net latency savings over a link of bandwidthBps
    [[nodiscard]] TransferEvaluation evaluate(std::span<const std::byte> data,
                                              uint64_t linkBandwidthBps) const noexcept;

    // Simplified decision predicate
    [[nodiscard]] bool shouldCompress(std::span<const std::byte> data,
                                     uint64_t linkBandwidthBps) const noexcept;

    // Records empirical compression timing to update rolling throughput estimates
    void recordCompression(size_t uncompressedBytes, size_t compressedBytes, uint64_t durationUs) noexcept;

    // Records empirical decompression timing to update rolling throughput estimates
    void recordDecompression(size_t uncompressedBytes, size_t compressedBytes, uint64_t durationUs) noexcept;

    // Reset or query statistics
    [[nodiscard]] CodecThroughputStats stats() const noexcept;
    void resetStats() noexcept;

    // Configuration
    void setForceCompression(std::optional<bool> force) noexcept;
    [[nodiscard]] std::optional<bool> forceCompression() const noexcept;

    void setMinPayloadBytes(size_t bytes) noexcept;
    [[nodiscard]] size_t minPayloadBytes() const noexcept;

private:
    mutable std::mutex           m_mu;
    mutable CodecThroughputStats m_stats;
    std::optional<bool>          m_force;
    size_t                       m_minPayloadBytes{1024};
};

// ─── Small-Message Batching Helper (Phase 2.6) ────────────────────────────────

struct BatchedPayload {
    [[nodiscard]] static std::vector<std::byte> pack(const std::vector<std::vector<std::byte>>& items);
    [[nodiscard]] static std::vector<std::vector<std::byte>> unpack(std::span<const std::byte> packed);
};

} // namespace dgpu::protocol
