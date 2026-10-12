// apps/worker/worker_client.cpp
// Reverse-channel client: heartbeats and status reporting from worker → host.

#include "worker_client.hpp"

#include <iostream>
#include <mutex>

namespace dgpu::worker {

WorkerClient::WorkerClient()  = default;
WorkerClient::~WorkerClient() { stop(); }

bool WorkerClient::connect(const std::string& hostAddress,
                            uint16_t           port,
                            uint32_t           /*timeoutMs*/) {
    bool ok = m_conn.connect(hostAddress, port);
    if (ok) {
        std::cout << "[worker_client] Reverse channel connected to "
                  << hostAddress << ':' << port << '\n';
    }
    return ok;
}

void WorkerClient::startHeartbeat(std::chrono::milliseconds interval) {
    m_running = true;
    m_heartbeatThread = std::thread([this, interval] {
        heartbeatLoop(interval);
    });
}

void WorkerClient::stop() {
    m_running = false;
    m_conn.close();
    if (m_heartbeatThread.joinable()) m_heartbeatThread.join();
}

void WorkerClient::updateStatus(const WorkerStatus& status) {
    std::lock_guard<std::mutex> lk(m_statusMu);
    m_status = status;
}

bool WorkerClient::isConnected() const noexcept {
    return m_conn.isConnected();
}

void WorkerClient::heartbeatLoop(std::chrono::milliseconds interval) {
    while (m_running && m_conn.isConnected()) {
        // Send a PING to confirm the host is still alive
        auto ping = protocol::Message::makePing(0);
        if (!m_conn.sendMessage(ping)) {
            std::cerr << "[worker_client] Heartbeat send failed — host disconnected\n";
            break;
        }
        protocol::Message pong;
        if (!m_conn.receiveMessage(pong)) {
            std::cerr << "[worker_client] Heartbeat receive failed — host disconnected\n";
            break;
        }
        std::this_thread::sleep_for(interval);
    }
    std::cout << "[worker_client] Heartbeat loop exited\n";
}

} // namespace dgpu::worker
