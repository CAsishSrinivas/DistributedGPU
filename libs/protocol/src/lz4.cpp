// protocol/lz4.cpp
// Clean, portable, self-contained LZ4 block compressor and decompressor.

#include "protocol/lz4.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace dgpu::protocol {

static constexpr uint32_t HASH_LOG  = 14;
static constexpr uint32_t HASH_SIZE = 1u << HASH_LOG; // 16384 entries
static constexpr uint32_t MIN_MATCH = 4;
static constexpr uint32_t MAX_DISTANCE = 65535;

static inline uint32_t hash4(uint32_t val) noexcept {
    return (val * 2654435761u) >> (32 - HASH_LOG);
}

static inline uint32_t read32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint16_t read16(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static inline void write16(uint8_t* p, uint16_t v) noexcept {
    std::memcpy(p, &v, sizeof(v));
}

size_t lz4CompressBound(size_t srcSize) noexcept {
    return srcSize + (srcSize / 255) + 16;
}

size_t lz4Compress(const void* src, size_t srcSize, void* dst, size_t dstCapacity) noexcept {
    if (srcSize == 0 || dstCapacity < 1) return 0;
    if (dstCapacity < lz4CompressBound(srcSize)) return 0;

    const auto* ip    = static_cast<const uint8_t*>(src);
    const auto* const iend  = ip + srcSize;
    const auto* const ilimit= (srcSize > 12) ? (iend - 12) : ip;
    const auto* anchor = ip;

    auto* op       = static_cast<uint8_t*>(dst);

    if (srcSize < 13) {
        // Too small to compress; write single literal block
        size_t litLen = srcSize;
        uint8_t* token = op++;
        if (litLen >= 15) {
            *token = 0xF0;
            size_t rem = litLen - 15;
            while (rem >= 255) {
                *op++ = 255;
                rem -= 255;
            }
            *op++ = static_cast<uint8_t>(rem);
        } else {
            *token = static_cast<uint8_t>(litLen << 4);
        }
        std::memcpy(op, ip, litLen);
        op += litLen;
        return static_cast<size_t>(op - static_cast<uint8_t*>(dst));
    }

    thread_local std::array<const uint8_t*, HASH_SIZE> hashTable;
    hashTable.fill(nullptr);

    while (ip < ilimit) {
        // Find match
        const uint8_t* match = nullptr;
        uint32_t forward = 1;
        while (true) {
            uint32_t h = hash4(read32(ip));
            match = hashTable[h];
            hashTable[h] = ip;

            if (match != nullptr && (ip - match) <= MAX_DISTANCE && (ip - match) > 0 && read32(match) == read32(ip)) {
                break; // Found match
            }
            ip += forward;
            if (ip >= ilimit) break;
        }

        if (ip >= ilimit) break;

        // Determine match length
        const uint8_t* m = match + 4;
        ip += 4;
        while (ip < iend && *ip == *m) {
            ++ip;
            ++m;
        }

        size_t matchLen = static_cast<size_t>(m - match);
        size_t litLen   = static_cast<size_t>(ip - matchLen - anchor);

        // Encode literal length and match length
        uint8_t* token = op++;
        uint8_t tokenVal = 0;

        if (litLen >= 15) {
            tokenVal |= 0xF0;
            size_t rem = litLen - 15;
            while (rem >= 255) {
                *op++ = 255;
                rem -= 255;
            }
            *op++ = static_cast<uint8_t>(rem);
        } else {
            tokenVal |= static_cast<uint8_t>(litLen << 4);
        }

        // Copy literals
        std::memcpy(op, anchor, litLen);
        op += litLen;

        // Write match offset (2 bytes LE)
        write16(op, static_cast<uint16_t>(ip - matchLen - match));
        op += 2;

        // Encode match length
        size_t remMatch = matchLen - 4;
        if (remMatch >= 15) {
            tokenVal |= 0x0F;
            remMatch -= 15;
            while (remMatch >= 255) {
                *op++ = 255;
                remMatch -= 255;
            }
            *op++ = static_cast<uint8_t>(remMatch);
        } else {
            tokenVal |= static_cast<uint8_t>(remMatch);
        }

        *token = tokenVal;
        anchor = ip;
    }

    // Write trailing literals
    size_t litLen = static_cast<size_t>(iend - anchor);
    if (litLen > 0) {
        uint8_t* token = op++;
        if (litLen >= 15) {
            *token = 0xF0;
            size_t rem = litLen - 15;
            while (rem >= 255) {
                *op++ = 255;
                rem -= 255;
            }
            *op++ = static_cast<uint8_t>(rem);
        } else {
            *token = static_cast<uint8_t>(litLen << 4);
        }
        std::memcpy(op, anchor, litLen);
        op += litLen;
    }

    return static_cast<size_t>(op - static_cast<uint8_t*>(dst));
}

size_t lz4Decompress(const void* src, size_t srcSize, void* dst, size_t dstCapacity) noexcept {
    if (srcSize == 0 || dstCapacity == 0) return 0;

    const auto* ip   = static_cast<const uint8_t*>(src);
    const auto* iend = ip + srcSize;
    auto*       op   = static_cast<uint8_t*>(dst);
    const auto* oend = op + dstCapacity;

    while (ip < iend) {
        uint8_t token = *ip++;
        size_t litLen = token >> 4;

        if (litLen == 15) {
            while (ip < iend && *ip == 255) {
                litLen += 255;
                ++ip;
            }
            if (ip < iend) {
                litLen += *ip++;
            }
        }

        if (op + litLen > oend || ip + litLen > iend) return 0; // Overrun
        std::memcpy(op, ip, litLen);
        op += litLen;
        ip += litLen;

        if (ip >= iend) break; // End of stream

        if (ip + 2 > iend) return 0;
        uint16_t offset = read16(ip);
        ip += 2;
        if (offset == 0 || op - offset < static_cast<uint8_t*>(dst)) return 0;

        size_t matchLen = (token & 0x0F) + 4;
        if ((token & 0x0F) == 15) {
            while (ip < iend && *ip == 255) {
                matchLen += 255;
                ++ip;
            }
            if (ip < iend) {
                matchLen += *ip++;
            }
        }

        if (op + matchLen > oend) return 0;

        // Byte-by-byte copy to handle overlapping matches correctly
        const uint8_t* match = op - offset;
        for (size_t i = 0; i < matchLen; ++i) {
            *op++ = *match++;
        }
    }

    return static_cast<size_t>(op - static_cast<uint8_t*>(dst));
}

std::vector<std::byte> compressBuffer(std::span<const std::byte> src) {
    if (src.empty()) return {};

    size_t bound = lz4CompressBound(src.size());
    // Prepend 4-byte uncompressed size header
    std::vector<std::byte> out(4 + bound);

    uint32_t uncompSize = static_cast<uint32_t>(src.size());
    std::memcpy(out.data(), &uncompSize, 4);

    size_t compBytes = lz4Compress(src.data(), src.size(),
                                   out.data() + 4, bound);
    if (compBytes == 0) {
        throw std::runtime_error("LZ4 compression failed");
    }
    out.resize(4 + compBytes);
    return out;
}

std::vector<std::byte> decompressBuffer(std::span<const std::byte> src, size_t uncompressedSize) {
    if (src.empty() || uncompressedSize == 0) return {};

    std::vector<std::byte> out(uncompressedSize);
    size_t decompBytes = lz4Decompress(src.data(), src.size(),
                                       out.data(), uncompressedSize);
    if (decompBytes != uncompressedSize) {
        throw std::runtime_error("LZ4 decompression failed or size mismatch");
    }
    return out;
}

} // namespace dgpu::protocol
