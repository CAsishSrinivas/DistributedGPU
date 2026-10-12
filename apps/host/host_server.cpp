// apps/host/host_server.cpp
// Manages the host-side server that listens for incoming worker connections
// and coordinates task submission across the distributed runtime.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "host_server.hpp"

#include "networking/tcp_connection.hpp"
#include "protocol/message.hpp"
#include "task_runtime/task_runtime.hpp"
#include "telemetry/telemetry.hpp"

#include <atomic>
#include <format>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace dgpu::host {

// ─── HostServer::Impl ────────────────────────────────────────────────────────

struct HostServer::Impl {
    networking::TaskServer          server;
    std::shared_ptr<WorkerProxy>    workerProxy;   // live remote connection
    std::mutex                      proxyMu;
    std::atomic_bool                running{false};
    uint16_t                        port{};
    ServerConfig                    cfg;
};

// ─── WorkerProxy — one per connected worker ───────────────────────────────────

WorkerProxy::WorkerProxy(networking::TcpConnection conn)
    : m_conn(std::move(conn))
    , m_connected(true)
{
    std::cout << "[host_server] Worker connected from "
              << m_conn.peerAddress() << '\n';
}

WorkerProxy::~WorkerProxy() { disconnect(); }

protocol::TaskResult WorkerProxy::submit(const protocol::TaskRequest& req) {
    std::lock_guard<std::mutex> lk(m_mu);

    // Build SUBMIT_TASK message
    auto payload = req.serialise();

    protocol::Message msg;
    msg.header.magic       = _byteswap_ulong(protocol::MAGIC);
    msg.header.version     = _byteswap_ushort(protocol::PROTO_VERSION);
    msg.header.messageType = _byteswap_ushort(
        static_cast<uint16_t>(protocol::MessageType::SubmitTask));
    msg.header.taskId      = _byteswap_uint64(req.taskId);
    msg.header.payloadSize = _byteswap_ulong(static_cast<uint32_t>(payload.size()));
    msg.header.status      = 0;
    msg.payload            = std::move(payload);

    if (!m_conn.sendMessage(msg)) {
        m_connected = false;
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    protocol::Message resp;
    if (!m_conn.receiveMessage(resp)) {
        m_connected = false;
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    uint32_t magic      = _byteswap_ulong(resp.header.magic);
    uint16_t version    = _byteswap_ushort(resp.header.version);
    uint16_t rawType    = _byteswap_ushort(resp.header.messageType);
    uint64_t respTaskId = _byteswap_uint64(resp.header.taskId);
    uint32_t respStatus = _byteswap_ulong(resp.header.status);

    if (magic != protocol::MAGIC || version != protocol::PROTO_VERSION ||
        (rawType != static_cast<uint16_t>(protocol::MessageType::TaskResult) &&
         rawType != static_cast<uint16_t>(protocol::MessageType::TaskError)) ||
        respTaskId != req.taskId) {
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }

    if (rawType == static_cast<uint16_t>(protocol::MessageType::TaskError) || respStatus != 0) {
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = (respStatus != 0) ? static_cast<protocol::TaskStatus>(respStatus)
                                       : protocol::TaskStatus::InternalError;
        return err;
    }

    try {
        return protocol::TaskResult::deserialise(resp.payload);
    } catch (...) {
        protocol::TaskResult err;
        err.taskId = req.taskId;
        err.status = protocol::TaskStatus::InternalError;
        return err;
    }
}

bool WorkerProxy::ping() {
    std::lock_guard<std::mutex> lk(m_mu);
    auto ping = protocol::Message::makePing(0);
    if (!m_conn.sendMessage(ping)) { m_connected = false; return false; }
    protocol::Message pong;
    if (!m_conn.receiveMessage(pong)) { m_connected = false; return false; }
    return true;
}

void WorkerProxy::disconnect() {
    m_connected = false;
    m_conn.close();
}

bool WorkerProxy::isConnected() const noexcept { return m_connected; }

// ─── HostServer ──────────────────────────────────────────────────────────────

HostServer::HostServer()  : m_impl(std::make_unique<Impl>()) {}
HostServer::~HostServer() { stop(); }

bool HostServer::start(uint16_t port, const ServerConfig& cfg) {
    m_impl->port = port;
    m_impl->cfg  = cfg;
    m_impl->running = true;

    bool ok = m_impl->server.start(port, [this](networking::TcpConnection conn) {
        onWorkerConnected(std::move(conn));
    });

    if (ok) std::cout << "[host_server] Listening for workers on port " << port << '\n';
    return ok;
}

void HostServer::stop() {
    m_impl->running = false;
    m_impl->server.stop();
    std::lock_guard<std::mutex> lk(m_impl->proxyMu);
    if (m_impl->workerProxy) {
        m_impl->workerProxy->disconnect();
        m_impl->workerProxy.reset();
    }
}

std::shared_ptr<WorkerProxy> HostServer::workerProxy() const {
    std::lock_guard<std::mutex> lk(m_impl->proxyMu);
    return m_impl->workerProxy;
}

bool HostServer::hasWorker() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->proxyMu);
    return m_impl->workerProxy && m_impl->workerProxy->isConnected();
}

void HostServer::onWorkerConnected(networking::TcpConnection conn) {
    // Perform HELLO handshake
    protocol::Message hello = protocol::Message::makeHello("HostA", "RTX 3050", 4096);
    if (!conn.sendMessage(hello)) return;

    protocol::Message ack;
    if (!conn.receiveMessage(ack)) return;

    uint32_t magic   = _byteswap_ulong(ack.header.magic);
    uint16_t version = _byteswap_ushort(ack.header.version);
    uint16_t type    = _byteswap_ushort(ack.header.messageType);

    if (magic != protocol::MAGIC || version != protocol::PROTO_VERSION ||
        type != static_cast<uint16_t>(protocol::MessageType::HelloAck)) {
        std::cerr << "[host_server] Handshake ACK rejected from " << conn.peerAddress() << '\n';
        conn.close();
        return;
    }

    auto proxy = std::make_shared<WorkerProxy>(std::move(conn));

    std::lock_guard<std::mutex> lk(m_impl->proxyMu);
    m_impl->workerProxy = proxy;
    std::cout << "[host_server] Worker proxy registered and verified for " << proxy->isConnected() << '\n';
}

} // namespace dgpu::host
