// protocol/message.cpp
// Serialisation / deserialisation for all protocol message types.

#include "protocol/message.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace dgpu::protocol {

// ─── Endian helpers ───────────────────────────────────────────────────────────

static uint32_t hton32(uint32_t v) noexcept {
#if defined(_WIN32)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
static uint32_t ntoh32(uint32_t v) noexcept { return hton32(v); }

static uint64_t hton64(uint64_t v) noexcept {
#if defined(_WIN32)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}
static uint64_t ntoh64(uint64_t v) noexcept { return hton64(v); }

static uint16_t hton16(uint16_t v) noexcept {
#if defined(_WIN32)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}
static uint16_t ntoh16(uint16_t v) noexcept { return hton16(v); }

// ─── Serialisation helpers ────────────────────────────────────────────────────

template<typename T>
static void appendPOD(std::vector<std::byte>& buf, T val) {
    const auto* p = reinterpret_cast<const std::byte*>(&val);
    buf.insert(buf.end(), p, p + sizeof(T));
}

template<typename T>
static T readPOD(std::span<const std::byte> data, size_t offset) {
    if (offset + sizeof(T) > data.size()) {
        throw std::runtime_error("Malformed payload: out-of-bounds read");
    }
    T val{};
    std::memcpy(&val, data.data() + offset, sizeof(T));
    return val;
}

static void appendString(std::vector<std::byte>& buf, const std::string& s) {
    uint32_t len = hton32(static_cast<uint32_t>(s.size()));
    appendPOD(buf, len);
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    buf.insert(buf.end(), p, p + s.size());
}

static std::string readString(std::span<const std::byte> data, size_t& off) {
    uint32_t len = ntoh32(readPOD<uint32_t>(data, off));
    off += sizeof(uint32_t);
    if (off + len > data.size()) throw std::runtime_error("String overrun in payload");
    std::string s(reinterpret_cast<const char*>(data.data() + off), len);
    off += len;
    return s;
}

// ─── Message factory methods ──────────────────────────────────────────────────

static Message makeControl(MessageType type, uint64_t taskId = 0,
                            TaskStatus status = TaskStatus::Ok) {
    Message m;
    m.header.magic       = hton32(MAGIC);
    m.header.version     = hton16(PROTO_VERSION);
    m.header.messageType = hton16(static_cast<uint16_t>(type));
    m.header.taskId      = hton64(taskId);
    m.header.payloadSize = 0;
    m.header.status      = hton32(static_cast<uint32_t>(status));
    return m;
}

Message Message::makeHello(const std::string& hostname,
                           const std::string& gpuName,
                           uint32_t           vramMb) {
    HelloPayload hp;
    hp.hostname = hostname;
    hp.gpuName  = gpuName;
    hp.vramMb   = vramMb;
    auto msg = makeControl(MessageType::Hello);
    msg.payload = hp.serialise();
    msg.header.payloadSize = hton32(static_cast<uint32_t>(msg.payload.size()));
    return msg;
}

Message Message::makeHelloAck(const std::string& hostname,
                              const std::string& gpuName,
                              uint32_t           vramMb) {
    HelloPayload hp;
    hp.hostname = hostname;
    hp.gpuName  = gpuName;
    hp.vramMb   = vramMb;
    auto msg = makeControl(MessageType::HelloAck);
    msg.payload = hp.serialise();
    msg.header.payloadSize = hton32(static_cast<uint32_t>(msg.payload.size()));
    return msg;
}

Message Message::makeTaskError(uint64_t taskId, TaskStatus status) {
    return makeControl(MessageType::TaskError, taskId, status);
}

Message Message::makePing(uint64_t taskId) { return makeControl(MessageType::Ping, taskId); }
Message Message::makePong(uint64_t taskId) { return makeControl(MessageType::Pong, taskId); }
Message Message::makeShutdown()            { return makeControl(MessageType::Shutdown); }

// ─── HelloPayload ─────────────────────────────────────────────────────────────

std::vector<std::byte> HelloPayload::serialise() const {
    std::vector<std::byte> buf;
    appendString(buf, hostname);
    appendString(buf, gpuName);
    appendPOD(buf, hton32(vramMb));
    return buf;
}

HelloPayload HelloPayload::deserialise(std::span<const std::byte> data) {
    HelloPayload hp;
    size_t off = 0;
    hp.hostname = readString(data, off);
    hp.gpuName  = readString(data, off);
    hp.vramMb   = ntoh32(readPOD<uint32_t>(data, off));
    off += sizeof(uint32_t);
    if (off != data.size()) {
        throw std::runtime_error("Malformed HelloPayload: unexpected trailing bytes");
    }
    return hp;
}

// ─── TaskRequest ─────────────────────────────────────────────────────────────

bool TaskRequest::validate() const noexcept {
    if (width == 0 || height == 0) return false;
    if (width > 4096 || height > 4096) return false;
    const size_t expected = static_cast<size_t>(width) * height * 4;
    if (input.size() != expected) return false;
    if (type != TaskType::Blur && type != TaskType::EdgeDetection) return false;
    return true;
}

std::vector<std::byte> TaskRequest::serialise() const {
    std::vector<std::byte> buf;
    appendPOD(buf, hton64(taskId));
    appendPOD(buf, hton32(static_cast<uint32_t>(type)));
    appendPOD(buf, hton32(width));
    appendPOD(buf, hton32(height));
    uint32_t sz = hton32(static_cast<uint32_t>(input.size()));
    appendPOD(buf, sz);
    buf.insert(buf.end(), input.begin(), input.end());
    return buf;
}

TaskRequest TaskRequest::deserialise(std::span<const std::byte> data) {
    TaskRequest req;
    size_t off = 0;
    req.taskId = ntoh64(readPOD<uint64_t>(data, off)); off += 8;
    req.type   = static_cast<TaskType>(ntoh32(readPOD<uint32_t>(data, off))); off += 4;
    req.width  = ntoh32(readPOD<uint32_t>(data, off)); off += 4;
    req.height = ntoh32(readPOD<uint32_t>(data, off)); off += 4;
    uint32_t sz = ntoh32(readPOD<uint32_t>(data, off)); off += 4;
    if (off + sz > data.size()) throw std::runtime_error("TaskRequest payload overrun");
    req.input.assign(data.begin() + static_cast<ptrdiff_t>(off),
                     data.begin() + static_cast<ptrdiff_t>(off + sz));
    off += sz;
    if (off != data.size()) {
        throw std::runtime_error("Malformed TaskRequest: unexpected trailing bytes");
    }
    if (!req.validate()) {
        throw std::runtime_error("Malformed TaskRequest: failed structural validation rules");
    }
    return req;
}

// ─── TaskResult ──────────────────────────────────────────────────────────────

std::vector<std::byte> TaskResult::serialise() const {
    std::vector<std::byte> buf;
    appendPOD(buf, hton64(taskId));
    appendPOD(buf, hton32(static_cast<uint32_t>(status)));
    appendPOD(buf, hton64(uploadUs));
    appendPOD(buf, hton64(gpuUs));
    appendPOD(buf, hton64(readbackUs));
    appendPOD(buf, hton64(totalUs));
    uint32_t sz = hton32(static_cast<uint32_t>(output.size()));
    appendPOD(buf, sz);
    buf.insert(buf.end(), output.begin(), output.end());
    return buf;
}

TaskResult TaskResult::deserialise(std::span<const std::byte> data) {
    TaskResult res;
    size_t off = 0;
    res.taskId     = ntoh64(readPOD<uint64_t>(data, off)); off += 8;
    auto rawStatus = ntoh32(readPOD<uint32_t>(data, off)); off += 4;
    if (rawStatus > static_cast<uint32_t>(TaskStatus::InternalError)) {
        throw std::runtime_error("Malformed TaskResult: invalid status enum value");
    }
    res.status = static_cast<TaskStatus>(rawStatus);
    res.uploadUs   = ntoh64(readPOD<uint64_t>(data, off)); off += 8;
    res.gpuUs      = ntoh64(readPOD<uint64_t>(data, off)); off += 8;
    res.readbackUs = ntoh64(readPOD<uint64_t>(data, off)); off += 8;
    res.totalUs    = ntoh64(readPOD<uint64_t>(data, off)); off += 8;
    uint32_t sz    = ntoh32(readPOD<uint32_t>(data, off)); off += 4;
    if (off + sz > data.size()) throw std::runtime_error("TaskResult payload overrun");
    res.output.assign(data.begin() + static_cast<ptrdiff_t>(off),
                      data.begin() + static_cast<ptrdiff_t>(off + sz));
    off += sz;
    if (off != data.size()) {
        throw std::runtime_error("Malformed TaskResult: unexpected trailing bytes");
    }
    return res;
}

} // namespace dgpu::protocol
