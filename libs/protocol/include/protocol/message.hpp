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

inline constexpr uint32_t MAGIC            = 0x44475055u; // "DGPU"
inline constexpr uint16_t PROTO_VERSION    = 2;
inline constexpr uint16_t PROTO_VERSION_V1 = 1;
inline constexpr uint32_t MAX_PAYLOAD      = 64u * 1024u * 1024u; // 64 MiB hard cap

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

// ─── Capability bitmask flags ────────────────────────────────────────────────

enum TaskCapabilityFlags : uint32_t {
    TaskCap_Blur          = (1u << static_cast<uint32_t>(TaskType::Blur)),
    TaskCap_EdgeDetection = (1u << static_cast<uint32_t>(TaskType::EdgeDetection)),
    TaskCap_All           = TaskCap_Blur | TaskCap_EdgeDetection,
};

// ─── Worker capabilities (Protocol v2) ───────────────────────────────────────

struct WorkerCapabilities {
    std::string workerId;
    std::string hostname;
    std::string gpuName;
    uint64_t    vramBytes{0};
    uint32_t    supportedTasks{TaskCap_All};
    uint32_t    maxWidth{4096};
    uint32_t    maxHeight{4096};
    uint32_t    maxBatchSize{64};

    [[nodiscard]] bool supportsTask(TaskType type, uint32_t width, uint32_t height) const noexcept {
        if (width == 0 || height == 0) return false;
        if (width > maxWidth || height > maxHeight) return false;
        const uint32_t bit = 1u << static_cast<uint32_t>(type);
        return (supportedTasks & bit) != 0;
    }

    [[nodiscard]] std::vector<std::byte> serialise() const;
    static WorkerCapabilities deserialise(std::span<const std::byte> data);
};

// ─── Wire header (packed, 24 bytes) ──────────────────────────────────────────

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;        // Must equal MAGIC
    uint16_t version;      // Must equal PROTO_VERSION or PROTO_VERSION_V1
    uint16_t messageType;  // MessageType cast to uint16_t
    uint64_t taskId;       // Correlation ID (0 for control messages)
    uint32_t payloadSize;  // Byte count of the payload that follows
    uint32_t status;       // TaskStatus (low 16 bits) and header flags (high 16 bits)
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 24, "MessageHeader must be exactly 24 bytes");

// ─── Header Flags (Phase 2.6) ────────────────────────────────────────────────
inline constexpr uint32_t FLAG_NONE        = 0x00000000u;
inline constexpr uint32_t FLAG_COMPRESSED  = 0x80000000u; // Payload is LZ4 compressed
inline constexpr uint32_t FLAG_BATCHED     = 0x40000000u; // Payload contains batched task messages
inline constexpr uint32_t FLAG_STATUS_MASK = 0x0000FFFFu;

[[nodiscard]] inline bool hasHeaderFlag(const MessageHeader& hdr, uint32_t flag) noexcept {
    uint32_t s = hdr.status;
#if defined(_WIN32)
    s = _byteswap_ulong(s);
#else
    s = __builtin_bswap32(s);
#endif
    return (s & flag) == flag;
}
[[nodiscard]] inline bool hasHeaderFlag(uint32_t hostStatus, uint32_t flag) noexcept {
    return (hostStatus & flag) == flag;
}
inline void setHeaderFlag(MessageHeader& hdr, uint32_t flag) noexcept {
    uint32_t s = hdr.status;
#if defined(_WIN32)
    s = _byteswap_ulong(s);
#else
    s = __builtin_bswap32(s);
#endif
    s |= flag;
#if defined(_WIN32)
    hdr.status = _byteswap_ulong(s);
#else
    hdr.status = __builtin_bswap32(s);
#endif
}
[[nodiscard]] inline TaskStatus getHeaderStatus(const MessageHeader& hdr) noexcept {
    uint32_t s = hdr.status;
#if defined(_WIN32)
    s = _byteswap_ulong(s);
#else
    s = __builtin_bswap32(s);
#endif
    return static_cast<TaskStatus>(s & FLAG_STATUS_MASK);
}

// ─── High-level message ───────────────────────────────────────────────────────

struct Message {
    MessageHeader            header{};
    std::vector<std::byte>   payload;

    // Convenience constructors
    static Message makeHello(const std::string& hostname,
                             const std::string& gpuName = "",
                             uint32_t           vramMb  = 0,
                             const WorkerCapabilities& caps = {});
    static Message makeHelloAck(const std::string& hostname = "WorkerB",
                                const std::string& gpuName  = "",
                                uint32_t           vramMb   = 0,
                                const WorkerCapabilities& caps = {});
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

    // Client-side breakdown (measured locally during dispatch, not transmitted on wire)
    uint64_t serializeUs{0};
    uint64_t compressUs{0};
    uint64_t sendFlightUs{0};
    uint64_t recvWaitUs{0};
    uint64_t decompressUs{0};
    uint64_t deserializeUs{0};
    uint64_t pureNetworkRttUs{0};
    size_t   uncompressedSendBytes{0};
    size_t   wireSendBytes{0};
    size_t   wireRecvBytes{0};
    size_t   uncompressedRecvBytes{0};

    [[nodiscard]] std::vector<std::byte> serialise() const;
    static TaskResult deserialise(std::span<const std::byte> data);
};

// ─── Hello payload ────────────────────────────────────────────────────────────

struct HelloPayload {
    std::string        hostname;
    std::string        gpuName;
    uint32_t           vramMb{};
    WorkerCapabilities capabilities{};

    [[nodiscard]] std::vector<std::byte> serialise() const;
    static HelloPayload deserialise(std::span<const std::byte> data);
};

} // namespace dgpu::protocol
