// protocol/lz4.hpp
// High-performance self-contained LZ4 block compression & decompression.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dgpu::protocol {

// Maximum compressed size for a given input buffer size
[[nodiscard]] size_t lz4CompressBound(size_t srcSize) noexcept;

// Compress src into dst. Returns bytes written, or 0 on error.
size_t lz4Compress(const void* src, size_t srcSize, void* dst, size_t dstCapacity) noexcept;

// Decompress src into dst. Returns bytes written, or 0 on error.
size_t lz4Decompress(const void* src, size_t srcSize, void* dst, size_t dstCapacity) noexcept;

// Vector convenience helpers
[[nodiscard]] std::vector<std::byte> compressBuffer(std::span<const std::byte> src);
[[nodiscard]] std::vector<std::byte> decompressBuffer(std::span<const std::byte> src, size_t uncompressedSize);

} // namespace dgpu::protocol
