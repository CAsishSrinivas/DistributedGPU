// tests/protocol_tests/test_protocol.cpp
// Unit tests for serialisation round-trips and validation rules.
// Uses no framework — just plain assertions so the test binary is zero-dependency.

#include "protocol/message.hpp"
#include "protocol/lz4.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

using namespace dgpu::protocol;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::cerr << "FAIL [" __FILE__ ":" << __LINE__ << "] " #expr << '\n'; \
            ++g_failed;                                                      \
        } else {                                                             \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

// ─── Serialisation round-trip ─────────────────────────────────────────────────

static void test_taskRequest_roundtrip() {
    TaskRequest original;
    original.taskId = 42;
    original.type   = TaskType::Blur;
    original.width  = 4;
    original.height = 4;
    original.input.resize(4 * 4 * 4);
    for (size_t i = 0; i < original.input.size(); ++i) {
        original.input[i] = static_cast<std::byte>(i & 0xFF);
    }

    auto bytes      = original.serialise();
    auto recovered  = TaskRequest::deserialise(bytes);

    CHECK(recovered.taskId == original.taskId);
    CHECK(recovered.type   == original.type);
    CHECK(recovered.width  == original.width);
    CHECK(recovered.height == original.height);
    CHECK(recovered.input  == original.input);
}

static void test_taskResult_roundtrip() {
    TaskResult original;
    original.taskId    = 99;
    original.status    = TaskStatus::Ok;
    original.uploadUs  = 111;
    original.gpuUs     = 222;
    original.readbackUs= 333;
    original.totalUs   = 666;
    original.output.resize(16, std::byte{0xAB});

    auto bytes     = original.serialise();
    auto recovered = TaskResult::deserialise(bytes);

    CHECK(recovered.taskId     == original.taskId);
    CHECK(recovered.status     == original.status);
    CHECK(recovered.uploadUs   == original.uploadUs);
    CHECK(recovered.gpuUs      == original.gpuUs);
    CHECK(recovered.readbackUs == original.readbackUs);
    CHECK(recovered.totalUs    == original.totalUs);
    CHECK(recovered.output     == original.output);
}

// ─── Validation ───────────────────────────────────────────────────────────────

static void test_validation_valid() {
    TaskRequest req;
    req.taskId = 1;
    req.type   = TaskType::Blur;
    req.width  = 8;
    req.height = 8;
    req.input.resize(8 * 8 * 4, std::byte{0});
    CHECK(req.validate());
}

static void test_validation_zero_dims() {
    TaskRequest req;
    req.taskId = 2;
    req.type   = TaskType::Blur;
    req.width  = 0;
    req.height = 8;
    req.input.resize(0);
    CHECK(!req.validate());
}

static void test_validation_oversized() {
    TaskRequest req;
    req.taskId = 3;
    req.type   = TaskType::Blur;
    req.width  = 5000; // > 4096
    req.height = 4;
    req.input.resize(static_cast<size_t>(5000) * 4 * 4, std::byte{0});
    CHECK(!req.validate());
}

static void test_validation_wrong_payload_size() {
    TaskRequest req;
    req.taskId = 4;
    req.type   = TaskType::EdgeDetection;
    req.width  = 4;
    req.height = 4;
    req.input.resize(10); // should be 64
    CHECK(!req.validate());
}

static void test_validation_unknown_type() {
    TaskRequest req;
    req.taskId = 5;
    req.type   = static_cast<TaskType>(0xDEAD);
    req.width  = 4;
    req.height = 4;
    req.input.resize(4 * 4 * 4, std::byte{0});
    CHECK(!req.validate());
}

// ─── HelloPayload round-trip ──────────────────────────────────────────────────

static void test_hello_roundtrip() {
    HelloPayload hp;
    hp.hostname = "LaptopA";
    hp.gpuName  = "NVIDIA GeForce RTX 3050";
    hp.vramMb   = 4096;

    auto bytes     = hp.serialise();
    auto recovered = HelloPayload::deserialise(bytes);

    CHECK(recovered.hostname == hp.hostname);
    CHECK(recovered.gpuName  == hp.gpuName);
    CHECK(recovered.vramMb   == hp.vramMb);
}

// ─── Header size guarantee ────────────────────────────────────────────────────

static void test_header_size() {
    CHECK(sizeof(MessageHeader) == 24);
}

static void test_malformed_truncated_payload() {
    // Truncated TaskRequest payload (e.g. only 10 bytes instead of minimum required header fields)
    std::vector<std::byte> truncated(10, std::byte{0});
    bool caught = false;
    try {
        TaskRequest::deserialise(truncated);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
}

static void test_trailing_bytes_rejected() {
    HelloPayload hp;
    hp.hostname = "LaptopA";
    hp.gpuName  = "RTX 3050";
    hp.vramMb   = 4096;

    auto bytes = hp.serialise();
    // Append 3 extra garbage trailing bytes
    bytes.push_back(std::byte{0xDE});
    bytes.push_back(std::byte{0xAD});
    bytes.push_back(std::byte{0xBE});

    bool caught = false;
    try {
        HelloPayload::deserialise(bytes);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
}

static void test_deserialise_invalid_dimensions_rejected() {
    // Construct serialised bytes with width=0
    std::vector<std::byte> raw(24, std::byte{0}); // 8 tid + 4 type + 4 w + 4 h + 4 sz = 24
    // set taskId=1, type=1 (Blur), width=0, height=10, sz=0
    uint64_t tid = _byteswap_uint64(1);
    uint32_t type = _byteswap_ulong(1);
    uint32_t w = 0;
    uint32_t h = _byteswap_ulong(10);
    uint32_t sz = 0;
    std::memcpy(raw.data(), &tid, 8);
    std::memcpy(raw.data() + 8, &type, 4);
    std::memcpy(raw.data() + 12, &w, 4);
    std::memcpy(raw.data() + 16, &h, 4);
    std::memcpy(raw.data() + 20, &sz, 4);

    bool caught = false;
    try {
        TaskRequest::deserialise(raw);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
}

static void test_taskResult_invalid_status_rejected() {
    TaskResult res;
    res.taskId = 10;
    res.status = TaskStatus::Ok;
    auto bytes = res.serialise();
    // Tamper status (at offset 8..11) to invalid value 9999
    uint32_t badStatus = _byteswap_ulong(9999);
    std::memcpy(bytes.data() + 8, &badStatus, 4);

    bool caught = false;
    try {
        TaskResult::deserialise(bytes);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
}

static void test_lz4_large_buffer_compression() {
    // Test 1 MB repetitive buffer (similar to game texture)
    const size_t rawSize = 1024 * 1024;
    std::vector<std::byte> raw(rawSize);
    for (size_t i = 0; i < rawSize; ++i) {
        raw[i] = static_cast<std::byte>((i / 64) % 256);
    }

    auto compressed = compressBuffer(raw);
    CHECK(!compressed.empty());
    // For repetitive data, compression should be substantial (at least 3x, so < 350 KB)
    CHECK(compressed.size() < rawSize / 3);

    auto decompressed = decompressBuffer(std::span<const std::byte>(compressed.data() + 4, compressed.size() - 4), rawSize);
    CHECK(decompressed.size() == rawSize);
    CHECK(decompressed == raw);
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Protocol Tests ===\n";
    test_header_size();
    test_taskRequest_roundtrip();
    test_taskResult_roundtrip();
    test_validation_valid();
    test_validation_zero_dims();
    test_validation_oversized();
    test_validation_wrong_payload_size();
    test_validation_unknown_type();
    test_hello_roundtrip();
    test_malformed_truncated_payload();
    test_trailing_bytes_rejected();
    test_deserialise_invalid_dimensions_rejected();
    test_taskResult_invalid_status_rejected();
    test_lz4_large_buffer_compression();

    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
