// protocol/message.hpp
// Defines every wire-format type for the DistributedGPU protocol (M2).
//
// Wire layout (little-endian):
//   [MessageHeader][payload bytes]
//
// All multi-byte integers are transmitted in network byte order (big-endian)
// and converted at both ends using the hton/ntoh helpers below.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dgpu::protocol {

// ─── Constants ───────────────────────────────────────────────────────────────

inline constexpr uint32_t MAGIC         = 0x44475055u; // "DGPU"
inline constexpr uint16_t PROTO_VERSION = 1;
inline constexpr uint32_t MAX_PAYLOAD   = 64u * 1024u * 1024u; // 64 MiB hard cap

// ─── Message types ───────────────────────────────────────────────────────────

enum class MessageType : uint16_t {
    Hello                = 0x0001,
    HelloAck             = 0x0002,
    SubmitTask           = 0x0010,
    TaskAccepted         = 0x0011,
    TaskResult           = 0x0012,
    TaskError            = 0x0013,
    SubmitTaskCompressed = 0x0014,
    TaskResultCompressed = 0x0015,
    Ping                 = 0x0020,
    Pong                 = 0x0021,
    Shutdown             = 0x00FF,
};

// ─── Task status codes ────────────────────────────────────────────────────────

enum class TaskStatus : uint32_t {
    Ok              = 0,
    InvalidRequest  = 1,
    UnsupportedTask = 2,
    GpuError        = 3,
    Timeout         = 4,
    InternalError   = 5,
};

// ─── Task types (the fixed registry — no arbitrary shader compilation) ────────

enum class TaskType : uint32_t {
    Blur          = 1,
    EdgeDetection = 2,
};

// ─── Wire header (packed, 24 bytes) ──────────────────────────────────────────

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;        // Must equal MAGIC
    uint16_t version;      // Must equal PROTO_VERSION
    uint16_t messageType;  // MessageType cast to uint16_t
    uint64_t taskId;       // Correlation ID (0 for control messages)
    uint32_t payloadSize;  // Byte count of the payload that follows
    uint32_t status;       // TaskStatus cast to uint32_t
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 24, "MessageHeader must be exactly 24 bytes");

// ─── High-level message ───────────────────────────────────────────────────────

struct Message {
    MessageHeader            header{};
    std::vector<std::byte>   payload;

    // Convenience constructors
    static Message makeHello(const std::string& hostname,
                             const std::string& gpuName = "",
                             uint32_t           vramMb  = 0);
    static Message makeHelloAck(const std::string& hostname = "WorkerB",
                                const std::string& gpuName  = "",
                                uint32_t           vramMb   = 0);
    static Message makeTaskError(uint64_t taskId, TaskStatus status);
    static Message makePing(uint64_t taskId = 0);
    static Message makePong(uint64_t taskId = 0);
    static Message makeShutdown();
};

// ─── Task request / result (serialised into payload) ─────────────────────────

struct TaskRequest {
    uint64_t             taskId{};
    TaskType             type{};
    uint32_t             width{};
    uint32_t             height{};
    std::vector<std::byte> input;   // RGBA8 pixel data: width * height * 4 bytes

    [[nodiscard]] bool validate() const noexcept;

    [[nodiscard]] std::vector<std::byte> serialise() const;
    static TaskRequest deserialise(std::span<const std::byte> data);
};

struct TaskResult {
    uint64_t             taskId{};
    TaskStatus           status{TaskStatus::Ok};
    std::vector<std::byte> output;  // Same layout as TaskRequest::input

    // Timing breakdown reported by the worker (all in microseconds)
    uint64_t uploadUs{};
    uint64_t gpuUs{};
    uint64_t readbackUs{};
    uint64_t totalUs{};

    [[nodiscard]] std::vector<std::byte> serialise() const;
    static TaskResult deserialise(std::span<const std::byte> data);
};

// ─── Hello payload ────────────────────────────────────────────────────────────

struct HelloPayload {
    std::string hostname;
    std::string gpuName;
    uint32_t    vramMb{};

    [[nodiscard]] std::vector<std::byte> serialise() const;
    static HelloPayload deserialise(std::span<const std::byte> data);
};

} // namespace dgpu::protocol
