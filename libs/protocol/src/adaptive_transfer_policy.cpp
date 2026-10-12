// protocol/adaptive_transfer_policy.cpp
// Adaptive compression policy, throughput modeling, and payload batching.

#include "protocol/adaptive_transfer_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace dgpu::protocol {

float AdaptiveTransferPolicy::estimateCompressibility(std::span<const std::byte> data) noexcept {
    if (data.size() < 256) return 1.0f;

    // Multi-point sampling: sample up to 4KB across 4 regions
    constexpr size_t kSampleChunk = 1024;
    constexpr size_t kMaxSamples  = 4;
    std::vector<std::byte> probeBuf;
    probeBuf.reserve(kSampleChunk * kMaxSamples);

    if (data.size() <= kSampleChunk * kMaxSamples) {
        probeBuf.assign(data.begin(), data.end());
    } else {
        size_t stride = (data.size() - kSampleChunk) / (kMaxSamples - 1);
        for (size_t s = 0; s < kMaxSamples; ++s) {
            size_t start = s * stride;
            probeBuf.insert(probeBuf.end(), data.begin() + start, data.begin() + start + kSampleChunk);
        }
    }

    auto comp = protocol::compressBuffer(probeBuf);
    if (comp.size() <= 4) return 1.0f;
    float ratio = static_cast<float>(probeBuf.size()) / static_cast<float>(comp.size());
    return ratio;
}

TransferEvaluation AdaptiveTransferPolicy::evaluate(std::span<const std::byte> data,
                                                    uint64_t linkBandwidthBps) const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    TransferEvaluation eval;
    const size_t sz = data.size();

    m_stats.totalBytesEvaluated += sz;

    if (m_force.has_value()) {
        eval.decision = m_force.value() ? CodecDecision::Compress : CodecDecision::BypassRaw;
        eval.isProfitable = m_force.value();
        eval.rationale = m_force.value() ? "Forced compression" : "Forced bypass";
        if (eval.decision == CodecDecision::Compress) {
            ++m_stats.compressDecisions;
        } else {
            ++m_stats.bypassDecisions;
            m_stats.totalBytesBypassed += sz;
        }
        return eval;
    }

    if (sz < m_minPayloadBytes) {
        eval.decision = CodecDecision::BypassRaw;
        eval.isProfitable = false;
        eval.rationale = std::format("Payload size ({} B) < minPayloadBytes ({} B); bypassed", sz, m_minPayloadBytes);
        ++m_stats.bypassDecisions;
        m_stats.totalBytesBypassed += sz;
        return eval;
    }

    if (linkBandwidthBps == 0) linkBandwidthBps = 12'500'000; // default 100 Mbps (~12.5 MB/s)

    float estRatio = estimateCompressibility(data);
    eval.estimatedRatio = estRatio;

    // Time to transmit raw payload over the wire (microseconds)
    double rawWireUs = (static_cast<double>(sz) / static_cast<double>(linkBandwidthBps)) * 1'000'000.0;
    eval.predictedRawUs = rawWireUs;

    // Time with compression: CPU compress + compressed wire + CPU decompress (microseconds)
    double compCpuUs   = (static_cast<double>(sz) / m_stats.compressSpeedBytesPerSec) * 1'000'000.0;
    double compBytes   = static_cast<double>(sz) / std::max(1.0, static_cast<double>(estRatio));
    double compWireUs  = (compBytes / static_cast<double>(linkBandwidthBps)) * 1'000'000.0;
    double decompCpuUs = (static_cast<double>(sz) / m_stats.decompressSpeedBytesPerSec) * 1'000'000.0;

    double totalCompUs = compCpuUs + compWireUs + decompCpuUs;
    eval.predictedCompressedUs = totalCompUs;
    eval.predictedSavingsUs = rawWireUs - totalCompUs;

    // Decision rule: Compress only if total turnaround time improves with >= 5% margin and ratio >= 1.10x
    if (totalCompUs < rawWireUs * 0.95 && estRatio >= 1.10f) {
        eval.decision = CodecDecision::Compress;
        eval.isProfitable = true;
        eval.rationale = std::format(
            "Compress profitable ({:.1f}x ratio): predicted {:.1f} us vs raw {:.1f} us ({:.1f} us saved)",
            estRatio, totalCompUs, rawWireUs, eval.predictedSavingsUs);
        ++m_stats.compressDecisions;
    } else {
        eval.decision = CodecDecision::BypassRaw;
        eval.isProfitable = false;
        eval.rationale = std::format(
            "Compress bypassed ({:.2f}x ratio, {:.1f} MB/s link): comp+decomp+wire ({:.1f} us) >= raw ({:.1f} us)",
            estRatio, static_cast<double>(linkBandwidthBps) / 1'000'000.0, totalCompUs, rawWireUs);
        ++m_stats.bypassDecisions;
        m_stats.totalBytesBypassed += sz;
    }

    return eval;
}

bool AdaptiveTransferPolicy::shouldCompress(std::span<const std::byte> data,
                                           uint64_t linkBandwidthBps) const noexcept {
    auto eval = evaluate(data, linkBandwidthBps);
    return eval.decision == CodecDecision::Compress;
}

void AdaptiveTransferPolicy::recordCompression(size_t uncompressedBytes,
                                              size_t compressedBytes,
                                              uint64_t durationUs) noexcept {
    if (uncompressedBytes == 0 || durationUs == 0) return;
    std::lock_guard<std::mutex> lk(m_mu);

    double sampleSpeed = (static_cast<double>(uncompressedBytes) / static_cast<double>(durationUs)) * 1'000'000.0;
    // EWMA update with alpha = 0.20
    m_stats.compressSpeedBytesPerSec = 0.20 * sampleSpeed + 0.80 * m_stats.compressSpeedBytesPerSec;

    if (compressedBytes > 0) {
        float sampleRatio = static_cast<float>(uncompressedBytes) / static_cast<float>(compressedBytes);
        m_stats.rollingCompressionRatio = (m_stats.rollingCompressionRatio <= 1.0f)
            ? sampleRatio
            : (0.20f * sampleRatio + 0.80f * m_stats.rollingCompressionRatio);
    }

    m_stats.totalBytesCompressed += uncompressedBytes;
    m_stats.totalCompressionUs += durationUs;
}

void AdaptiveTransferPolicy::recordDecompression(size_t uncompressedBytes,
                                                size_t /*compressedBytes*/,
                                                uint64_t durationUs) noexcept {
    if (uncompressedBytes == 0 || durationUs == 0) return;
    std::lock_guard<std::mutex> lk(m_mu);

    double sampleSpeed = (static_cast<double>(uncompressedBytes) / static_cast<double>(durationUs)) * 1'000'000.0;
    m_stats.decompressSpeedBytesPerSec = 0.20 * sampleSpeed + 0.80 * m_stats.decompressSpeedBytesPerSec;
    m_stats.totalDecompressionUs += durationUs;
}

CodecThroughputStats AdaptiveTransferPolicy::stats() const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_stats;
}

void AdaptiveTransferPolicy::resetStats() noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    m_stats = CodecThroughputStats{};
}

void AdaptiveTransferPolicy::setForceCompression(std::optional<bool> force) noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    m_force = force;
}

std::optional<bool> AdaptiveTransferPolicy::forceCompression() const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_force;
}

void AdaptiveTransferPolicy::setMinPayloadBytes(size_t bytes) noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    m_minPayloadBytes = bytes;
}

size_t AdaptiveTransferPolicy::minPayloadBytes() const noexcept {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_minPayloadBytes;
}

// ─── Small-Message Batching Helper (Phase 2.6) ────────────────────────────────

std::vector<std::byte> BatchedPayload::pack(const std::vector<std::vector<std::byte>>& items) {
    uint32_t count = static_cast<uint32_t>(items.size());
    size_t totalBytes = 4;
    for (const auto& item : items) {
        totalBytes += 4 + item.size();
    }

    std::vector<std::byte> buf(totalBytes);
    std::memcpy(buf.data(), &count, 4);

    size_t offset = 4;
    for (const auto& item : items) {
        uint32_t len = static_cast<uint32_t>(item.size());
        std::memcpy(buf.data() + offset, &len, 4);
        offset += 4;
        if (!item.empty()) {
            std::memcpy(buf.data() + offset, item.data(), item.size());
            offset += item.size();
        }
    }
    return buf;
}

std::vector<std::vector<std::byte>> BatchedPayload::unpack(std::span<const std::byte> packed) {
    if (packed.empty()) return {};
    if (packed.size() < 4) {
        throw std::runtime_error("Malformed batched payload: header truncated");
    }
    uint32_t count = 0;
    std::memcpy(&count, packed.data(), 4);

    std::vector<std::vector<std::byte>> items;
    items.reserve(count);

    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + 4 > packed.size()) {
            throw std::runtime_error("Malformed batched payload: unexpected EOF reading item length");
        }
        uint32_t len = 0;
        std::memcpy(&len, packed.data() + offset, 4);
        offset += 4;
        if (offset + len > packed.size()) {
            throw std::runtime_error("Malformed batched payload: item payload overrun");
        }

        std::vector<std::byte> item(packed.begin() + offset, packed.begin() + offset + len);
        offset += len;
        items.push_back(std::move(item));
    }
    if (offset != packed.size()) {
        throw std::runtime_error("Malformed batched payload: unexpected trailing bytes");
    }
    return items;
}

} // namespace dgpu::protocol
