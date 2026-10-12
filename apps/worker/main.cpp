// apps/worker/main.cpp
// Worker process: initialises the GTX 1650 Ti, listens for task submissions,
// executes them on the GPU, and returns results with timing breakdowns.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "networking/tcp_connection.hpp"
#include "networking/transport_config.hpp"
#include "networking/interface_discovery.hpp"
#include "protocol/message.hpp"
#include "protocol/lz4.hpp"
#include "d3d12/gpu_executor.hpp"
#include "telemetry/telemetry.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

// ─── Configuration ────────────────────────────────────────────────────────────

static constexpr uint16_t    WORKER_PORT      = 7777;
static constexpr const char* PREFERRED_ADAPTER = "GTX 1650";  // substring match

// ─── Global state ─────────────────────────────────────────────────────────────

static std::atomic_bool                   g_running{true};
static dgpu::protocol::WorkerCapabilities s_workerCaps;

// ─── Stable Worker UUID ───────────────────────────────────────────────────────

static std::string getOrCreateWorkerId() {
    const std::filesystem::path idPath = "worker_id.txt";
    if (std::filesystem::exists(idPath)) {
        std::ifstream f(idPath);
        std::string id;
        if (f >> id && !id.empty()) {
            return id;
        }
    }

    GUID guid{};
    std::string newId;
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            guid.Data1, guid.Data2, guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        newId = buf;
    } else {
        newId = "00000000-0000-0000-0000-000000000001";
    }

    std::ofstream out(idPath);
    if (out.is_open()) {
        out << newId;
    }
    return newId;
}

static void handleSignal(int) {
    std::cout << "\n[worker] Shutdown requested.\n";
    g_running = false;
}

// ─── Connection handler ───────────────────────────────────────────────────────

static void handleConnection(dgpu::networking::TcpConnection conn,
                              dgpu::gpu_executor::GpuExecutor& executor) {
    using namespace dgpu::protocol;
    std::cout << "[worker] Client connected from " << conn.peerAddress() << '\n';

    // Perform handshake
    Message hello;
    if (!conn.receiveMessage(hello)) {
        std::cerr << "[worker] Failed to receive HELLO handshake from " << conn.peerAddress() << '\n';
        return;
    }

    uint32_t magic   = _byteswap_ulong(hello.header.magic);
    uint16_t version = _byteswap_ushort(hello.header.version);
    uint16_t msgType = _byteswap_ushort(hello.header.messageType);

    if (magic != MAGIC ||
        (version != PROTO_VERSION && version != PROTO_VERSION_V1) ||
        msgType != static_cast<uint16_t>(MessageType::Hello)) {
        std::cerr << "[worker] Handshake rejected: invalid header from " << conn.peerAddress()
                  << " (magic=0x" << std::hex << magic << " type=0x" << msgType << std::dec << ")\n";
        auto err = Message::makeTaskError(0, TaskStatus::InvalidRequest);
        (void)conn.sendMessage(err);
        return;
    }

    HelloPayload hostInfo;
    try {
        if (!hello.payload.empty()) {
            hostInfo = HelloPayload::deserialise(hello.payload);
        }
    } catch (const std::exception& e) {
        std::cerr << "[worker] Malformed HELLO payload from " << conn.peerAddress() << ": " << e.what() << '\n';
        auto err = Message::makeTaskError(0, TaskStatus::InvalidRequest);
        (void)conn.sendMessage(err);
        return;
    }

    std::cout << "[worker] Valid HELLO handshake from: "
              << (hostInfo.hostname.empty() ? conn.peerAddress() : hostInfo.hostname)
              << " (" << (hostInfo.gpuName.empty() ? "GPU unspecified" : hostInfo.gpuName) << ")\n";

    const auto& adapter = executor.adapterInfo();
    uint32_t vramMb = static_cast<uint32_t>(adapter.dedicatedVideoMemoryBytes / (1024 * 1024));
    Message ack = Message::makeHelloAck("WorkerB", adapter.name, vramMb, s_workerCaps);
    if (!conn.sendMessage(ack)) {
        std::cerr << "[worker] Failed to send HELLO_ACK\n";
        return;
    }

    // Main message loop
    while (g_running) {
        Message msg;
        if (!conn.receiveMessage(msg)) break;

        uint16_t rawType = 0;
        std::memcpy(&rawType, &msg.header.messageType, sizeof(rawType));
        rawType = _byteswap_ushort(rawType);
        auto type = static_cast<MessageType>(rawType);

        if (type == MessageType::Shutdown) {
            std::cout << "[worker] Client sent SHUTDOWN\n";
            break;
        }

        if (type == MessageType::Ping) {
            uint64_t tid = 0;
            std::memcpy(&tid, &msg.header.taskId, sizeof(tid));
            tid = _byteswap_uint64(tid);
            auto pong = Message::makePong(tid);
            (void)conn.sendMessage(pong);
            continue;
        }

        if (type == MessageType::SubmitTask || type == MessageType::SubmitTaskCompressed) {
            bool isCompressed = (type == MessageType::SubmitTaskCompressed) ||
                                dgpu::protocol::hasHeaderFlag(msg.header, dgpu::protocol::FLAG_COMPRESSED);
            // Deserialise and validate
            TaskRequest req;
            try {
                if (isCompressed) {
                    if (msg.payload.size() < 4) {
                        throw std::runtime_error("Compressed task payload too small");
                    }
                    uint32_t uncompSize = 0;
                    std::memcpy(&uncompSize, msg.payload.data(), 4);
                    auto decomp = dgpu::protocol::decompressBuffer(
                        std::span<const std::byte>(msg.payload.data() + 4, msg.payload.size() - 4),
                        uncompSize);
                    req = TaskRequest::deserialise(decomp);
                } else {
                    req = TaskRequest::deserialise(msg.payload);
                }
            } catch (const std::exception& e) {
                std::cerr << "[worker] Bad task payload: " << e.what() << '\n';
                // Send error
                TaskResult err;
                err.taskId = 0;
                err.status = TaskStatus::InvalidRequest;
                Message resp;
                auto pl = err.serialise();
                resp.header.magic       = _byteswap_ulong(MAGIC);
                resp.header.version     = _byteswap_ushort(PROTO_VERSION);
                resp.header.messageType = _byteswap_ushort(
                    static_cast<uint16_t>(MessageType::TaskError));
                resp.header.taskId      = 0;
                resp.header.payloadSize = _byteswap_ulong(
                    static_cast<uint32_t>(pl.size()));
                resp.header.status      = _byteswap_ulong(
                    static_cast<uint32_t>(TaskStatus::InvalidRequest));
                resp.payload            = std::move(pl);
                (void)conn.sendMessage(resp);
                continue;
            }

            // Validate against worker capabilities
            if (!s_workerCaps.supportsTask(req.type, req.width, req.height)) {
                std::cerr << "[worker] Task " << req.taskId << " rejected: unsupported task type="
                          << static_cast<uint32_t>(req.type) << " or bounds "
                          << req.width << "x" << req.height << '\n';
                TaskResult err;
                err.taskId = req.taskId;
                err.status = TaskStatus::UnsupportedTask;
                Message resp;
                auto pl = err.serialise();
                resp.header.magic       = _byteswap_ulong(MAGIC);
                resp.header.version     = _byteswap_ushort(PROTO_VERSION);
                resp.header.messageType = _byteswap_ushort(
                    static_cast<uint16_t>(MessageType::TaskError));
                resp.header.taskId      = _byteswap_uint64(req.taskId);
                resp.header.payloadSize = _byteswap_ulong(
                    static_cast<uint32_t>(pl.size()));
                resp.header.status      = _byteswap_ulong(
                    static_cast<uint32_t>(TaskStatus::UnsupportedTask));
                resp.payload            = std::move(pl);
                (void)conn.sendMessage(resp);
                continue;
            }

            // Execute on GPU
            TaskResult result = executor.execute(req);

            static uint32_t s_taskCount = 0;
            if (++s_taskCount % 60 == 1) {
                std::cout << std::format("[worker] Task {} done — upload={}us gpu={}us readback={}us total={}us\n",
                    result.taskId, result.uploadUs, result.gpuUs,
                    result.readbackUs, result.totalUs);
            }

            // Record telemetry
            dgpu::telemetry::FrameRecord fr;
            fr.label     = "worker";
            fr.isRemote  = false;
            fr.uploadUs  = result.uploadUs;
            fr.gpuExecUs = result.gpuUs;
            fr.readbackUs= result.readbackUs;
            fr.totalUs   = result.totalUs;
            fr.width     = req.width;
            fr.height    = req.height;
            dgpu::telemetry::Telemetry::instance().record(fr);

            // Send result back
            Message resp;
            auto pl = result.serialise();
            bool returnCompressed = isCompressed && (result.status == TaskStatus::Ok);
            if (returnCompressed) {
                pl = dgpu::protocol::compressBuffer(pl);
            }
            auto respType = (result.status != TaskStatus::Ok)
                                ? MessageType::TaskError
                                : (returnCompressed ? MessageType::TaskResultCompressed
                                                    : MessageType::TaskResult);
            resp.header.magic       = _byteswap_ulong(MAGIC);
            resp.header.version     = _byteswap_ushort(PROTO_VERSION);
            resp.header.messageType = _byteswap_ushort(static_cast<uint16_t>(respType));
            resp.header.taskId      = _byteswap_uint64(result.taskId);
            resp.header.payloadSize = _byteswap_ulong(static_cast<uint32_t>(pl.size()));
            resp.header.status      = _byteswap_ulong(static_cast<uint32_t>(result.status));
            if (returnCompressed) {
                dgpu::protocol::setHeaderFlag(resp.header, dgpu::protocol::FLAG_COMPRESSED);
            }
            resp.payload            = std::move(pl);
            (void)conn.sendMessage(resp);
            continue;
        }

        std::cerr << "[worker] Unknown message type " << rawType << '\n';
    }

    std::cout << "[worker] Client disconnected: " << conn.peerAddress() << '\n';
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::cout << "=== DistributedGPU Worker ===\n";

    // Command-line options & positional compatibility: worker.exe [options] [port] [iterations]
    uint16_t    port = WORKER_PORT;
    std::string bindIp = "0.0.0.0";
    std::string interfaceName = "";
    bool        hasPositionalPort = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: worker.exe [options] [port] [iterations]\n\n"
                      << "Options:\n"
                      << "  -h, --help           Show this help message and exit\n"
                      << "  --port <port>        TCP port to listen on (default: 7777)\n"
                      << "  --bind-ip <ip>       Local interface IP to bind on (default: 0.0.0.0)\n"
                      << "  --interface <name>   Local interface name to bind exclusively on (e.g. \"USB4\")\n"
                      << "  --iterations <n>     Target compute shader iterations\n"
                      << "  --list-interfaces    List available network adapters and exit\n\n"
                      << "Positional arguments (backward compatible):\n"
                      << "  [port]               TCP listening port\n"
                      << "  [iterations]         Target compute shader iterations\n";
            return 0;
        } else if (arg == "--list-interfaces") {
            auto ifaces = dgpu::networking::InterfaceDiscovery::enumerateInterfaces();
            std::cout << "Available Network Interfaces:\n";
            for (const auto& iface : ifaces) {
                std::cout << "  - " << iface.friendlyName << " [" << dgpu::networking::interfaceTypeToString(iface.type) << "]"
                          << (iface.isUp ? " (UP)" : " (DOWN)")
                          << " IP: " << (iface.ipv4Address.empty() ? "none" : iface.ipv4Address)
                          << " Speed: " << (iface.linkSpeedBps / 1'000'000) << " Mbps\n"
                          << "    Desc: " << iface.description << "\n";
            }
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bind-ip" && i + 1 < argc) {
            bindIp = argv[++i];
        } else if (arg == "--interface" && i + 1 < argc) {
            interfaceName = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            SetEnvironmentVariableA("DGPU_ITERATIONS", argv[++i]);
            std::cout << "[worker] Target compute iterations set via CLI: " << argv[i] << "\n";
        } else if (!arg.starts_with("-")) {
            if (!hasPositionalPort) {
                port = static_cast<uint16_t>(std::stoi(arg));
                hasPositionalPort = true;
            } else {
                SetEnvironmentVariableA("DGPU_ITERATIONS", arg.c_str());
                std::cout << "[worker] Target compute iterations set via CLI: " << arg << "\n";
            }
        }
    }

    if (bindIp == "0.0.0.0" && !interfaceName.empty()) {
        auto matches = dgpu::networking::InterfaceDiscovery::findInterfaces(interfaceName);
        for (const auto& iface : matches) {
            if (iface.isUp && !iface.ipv4Address.empty()) {
                bindIp = iface.ipv4Address;
                break;
            }
        }
    }

    // Check dgpu.ini for local configuration
    std::ifstream cfg("dgpu.ini");
    if (cfg.is_open()) {
        std::string line;
        while (std::getline(cfg, line)) {
            if (line.find("iterations=") != std::string::npos) {
                std::string iters = line.substr(line.find("iterations=") + 11);
                SetEnvironmentVariableA("DGPU_ITERATIONS", iters.c_str());
                std::cout << "[worker] Configured iterations from dgpu.ini: " << iters << "\n";
            }
        }
    }

    // Winsock
    dgpu::networking::WinsockInit wsa;
    if (!wsa.ok()) {
        std::cerr << "[worker] Winsock init failed\n";
        return 1;
    }

    // GPU
    dgpu::gpu_executor::GpuExecutor executor;
    std::cout << "[worker] Initialising GPU...\n";
    if (!executor.init(PREFERRED_ADAPTER)) {
        std::cerr << "[worker] GPU init failed — check adapter name and drivers\n";
        return 2;
    }
    std::cout << "[worker] GPU ready: " << executor.adapterInfo().name << '\n';

    // Populate worker capabilities
    const auto& adapter = executor.adapterInfo();
    s_workerCaps.workerId = getOrCreateWorkerId();
    char hostBuf[256] = {};
    DWORD hostLen = sizeof(hostBuf);
    if (GetComputerNameA(hostBuf, &hostLen)) {
        s_workerCaps.hostname = hostBuf;
    } else {
        s_workerCaps.hostname = "WorkerB";
    }
    s_workerCaps.gpuName        = adapter.name;
    s_workerCaps.vramBytes      = adapter.dedicatedVideoMemoryBytes;
    s_workerCaps.supportedTasks = dgpu::protocol::TaskCap_Blur | dgpu::protocol::TaskCap_EdgeDetection;
    s_workerCaps.maxWidth       = 4096;
    s_workerCaps.maxHeight      = 4096;
    s_workerCaps.maxBatchSize   = 64;

    std::cout << "[worker] Registered Worker ID: " << s_workerCaps.workerId << '\n';
    std::cout << "[worker] Capabilities: GPU=\"" << s_workerCaps.gpuName
              << "\" VRAM=" << (s_workerCaps.vramBytes / (1024 * 1024)) << " MB, "
              << "Max Resolution=" << s_workerCaps.maxWidth << "x" << s_workerCaps.maxHeight << '\n';

    // Signal handling
    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    // Start server
    dgpu::networking::TaskServer server;
    bool started = server.start(port, [&executor](dgpu::networking::TcpConnection conn) {
        handleConnection(std::move(conn), executor);
    }, bindIp);

    if (!started) {
        std::cerr << "[worker] Failed to bind on " << bindIp << ":" << port << '\n';
        return 3;
    }

    std::cout << "[worker] Listening on " << bindIp << ":" << port;
    if (!interfaceName.empty()) std::cout << " (interface: " << interfaceName << ")";
    std::cout << " — press Ctrl+C to stop\n";

    // Wait for shutdown
    while (g_running) {
        Sleep(500);
    }

    server.stop();

    // Export telemetry
    auto csvPath = std::filesystem::path("worker_telemetry.csv");
    if (dgpu::telemetry::Telemetry::instance().exportCsv(csvPath)) {
        std::cout << "[worker] Telemetry saved to " << csvPath.string() << '\n';
    }

    std::cout << "[worker] Goodbye.\n";
    return 0;
}
