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

// ─── Worker Capabilities & Protocol v2 Tests ─────────────────────────────────

static void test_workerCapabilities_roundtrip() {
    WorkerCapabilities caps;
    caps.workerId       = "12345678-abcd-ef01-2345-6789abcdef01";
    caps.hostname       = "Laptop-B";
    caps.gpuName        = "NVIDIA GeForce GTX 1650 Ti";
    caps.vramBytes      = 4ull * 1024ull * 1024ull * 1024ull;
    caps.supportedTasks = TaskCap_Blur;
    caps.maxWidth       = 1920;
    caps.maxHeight      = 1080;
    caps.maxBatchSize   = 32;

    auto bytes = caps.serialise();
    auto recovered = WorkerCapabilities::deserialise(bytes);

    CHECK(recovered.workerId       == caps.workerId);
    CHECK(recovered.hostname       == caps.hostname);
    CHECK(recovered.gpuName        == caps.gpuName);
    CHECK(recovered.vramBytes      == caps.vramBytes);
    CHECK(recovered.supportedTasks == caps.supportedTasks);
    CHECK(recovered.maxWidth       == caps.maxWidth);
    CHECK(recovered.maxHeight      == caps.maxHeight);
    CHECK(recovered.maxBatchSize   == caps.maxBatchSize);
}

static void test_workerCapabilities_supportsTask() {
    WorkerCapabilities caps;
    caps.supportedTasks = TaskCap_Blur; // Only Blur supported
    caps.maxWidth       = 1920;
    caps.maxHeight      = 1080;

    // Supported within bounds
    CHECK(caps.supportsTask(TaskType::Blur, 1920, 1080));
    CHECK(caps.supportsTask(TaskType::Blur, 1280, 720));

    // Unsupported task type
    CHECK(!caps.supportsTask(TaskType::EdgeDetection, 1920, 1080));

    // Exceeding resolution bounds
    CHECK(!caps.supportsTask(TaskType::Blur, 2560, 1440));
    CHECK(!caps.supportsTask(TaskType::Blur, 1920, 1081));
    CHECK(!caps.supportsTask(TaskType::Blur, 0, 1080));
    CHECK(!caps.supportsTask(TaskType::Blur, 1920, 0));
}

static void test_hello_v2_capabilities_roundtrip() {
    WorkerCapabilities caps;
    caps.workerId       = "uuid-9876";
    caps.hostname       = "WorkerHost";
    caps.gpuName        = "RTX 3050";
    caps.vramBytes      = 4096ull * 1024ull * 1024ull;
    caps.supportedTasks = TaskCap_Blur | TaskCap_EdgeDetection;
    caps.maxWidth       = 3840;
    caps.maxHeight      = 2160;
    caps.maxBatchSize   = 64;

    HelloPayload hp;
    hp.hostname     = "WorkerHost";
    hp.gpuName      = "RTX 3050";
    hp.vramMb       = 4096;
    hp.capabilities = caps;

    auto bytes = hp.serialise();
    auto rec   = HelloPayload::deserialise(bytes);

    CHECK(rec.hostname == "WorkerHost");
    CHECK(rec.gpuName  == "RTX 3050");
    CHECK(rec.vramMb   == 4096);
    CHECK(rec.capabilities.workerId     == "uuid-9876");
    CHECK(rec.capabilities.vramBytes    == 4096ull * 1024ull * 1024ull);
    CHECK(rec.capabilities.supportedTasks == (TaskCap_Blur | TaskCap_EdgeDetection));
    CHECK(rec.capabilities.maxWidth     == 3840);
    CHECK(rec.capabilities.maxHeight    == 2160);
    CHECK(rec.capabilities.maxBatchSize == 64);
}

static void test_hello_v1_backward_compatibility() {
    // Construct wire buffer in exact Protocol v1 format (3 fields, no trailing capability bytes)
    std::vector<std::byte> v1Buf;
    auto appendStr = [&](const std::string& s) {
        uint32_t len = _byteswap_ulong(static_cast<uint32_t>(s.size()));
        const auto* pLen = reinterpret_cast<const std::byte*>(&len);
        v1Buf.insert(v1Buf.end(), pLen, pLen + 4);
        const auto* pStr = reinterpret_cast<const std::byte*>(s.data());
        v1Buf.insert(v1Buf.end(), pStr, pStr + s.size());
    };
    appendStr("LegacyWorker");
    appendStr("GTX 1060");
    uint32_t vram = _byteswap_ulong(3072);
    const auto* pVram = reinterpret_cast<const std::byte*>(&vram);
    v1Buf.insert(v1Buf.end(), pVram, pVram + 4);

    // Deserialise should succeed without throwing and populate sensible defaults
    HelloPayload hp = HelloPayload::deserialise(v1Buf);
    CHECK(hp.hostname == "LegacyWorker");
    CHECK(hp.gpuName  == "GTX 1060");
    CHECK(hp.vramMb   == 3072);
    CHECK(hp.capabilities.hostname == "LegacyWorker");
    CHECK(hp.capabilities.gpuName  == "GTX 1060");
    CHECK(hp.capabilities.vramBytes == 3072ull * 1024ull * 1024ull);
    CHECK(hp.capabilities.supportsTask(TaskType::Blur, 1920, 1080));
    CHECK(hp.capabilities.supportsTask(TaskType::EdgeDetection, 1920, 1080));
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
    test_workerCapabilities_roundtrip();
    test_workerCapabilities_supportsTask();
    test_hello_v2_capabilities_roundtrip();
    test_hello_v1_backward_compatibility();

    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
