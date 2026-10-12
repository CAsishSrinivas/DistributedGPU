// apps/game_adapter/dxgi_proxy.cpp
// Milestone M7: Live DXGI Proxy Hook for DirectX 12 games (e.g. Elden Ring, Shadow of the Tomb Raider).
// Compiles to dxgi.dll. When placed in the game executable's root folder,
// Windows loads this DLL before System32\dxgi.dll.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "d3d12_game_hook.hpp"
#include "../host/job_scheduler.hpp"
#include "../host/frame_compositor.hpp"
#include "networking/tcp_connection.hpp"
#include "networking/transport_config.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <deque>

using Microsoft::WRL::ComPtr;

namespace {

// ─── File & Debug Logger ──────────────────────────────────────────────────────
void logMsg(const std::string& msg) {
    OutputDebugStringA((msg + "\n").c_str());
    static std::mutex s_logMu;
    std::lock_guard<std::mutex> lk(s_logMu);
    static std::ofstream s_log("dgpu_dxgi.log", std::ios::app);
    if (s_log.is_open()) {
        s_log << msg << std::endl;
    }
}

// ─── Real System DXGI Function Pointers ───────────────────────────────────────
using PFN_CreateDXGIFactory                 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory1                = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2                = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_DXGIDeclareAdapterRemovalSupport  = HRESULT(WINAPI*)();
using PFN_DXGIGetDebugInterface1           = HRESULT(WINAPI*)(UINT, REFIID, void**);

PFN_CreateDXGIFactory                g_realCreateDXGIFactory                = nullptr;
PFN_CreateDXGIFactory1               g_realCreateDXGIFactory1               = nullptr;
PFN_CreateDXGIFactory2               g_realCreateDXGIFactory2               = nullptr;
PFN_DXGIDeclareAdapterRemovalSupport g_realDXGIDeclareAdapterRemovalSupport = nullptr;
PFN_DXGIGetDebugInterface1          g_realDXGIGetDebugInterface1          = nullptr;

HMODULE g_systemDxgiModule = nullptr;

// ─── Intercepted COM Method Types ────────────────────────────────────────────
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory* This,
    IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain);

using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain);

using PFN_CreateSwapChainForCoreWindow = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain);

using PFN_CreateSwapChainForComposition = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain);

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain* This,
    UINT SyncInterval,
    UINT Flags);

using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain1* This,
    UINT SyncInterval,
    UINT Flags,
    const DXGI_PRESENT_PARAMETERS* pPresentParameters);

typedef HRESULT (STDMETHODCALLTYPE *PFN_ResizeBuffers)(
    IDXGISwapChain* This,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags);

using PFN_ResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3* This,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags,
    const UINT* pCreationNodeMask,
    IUnknown* const* ppPresentQueue);

PFN_CreateSwapChain               g_realCreateSwapChain               = nullptr;
PFN_CreateSwapChainForHwnd        g_realCreateSwapChainForHwnd        = nullptr;
PFN_CreateSwapChainForCoreWindow  g_realCreateSwapChainForCoreWindow  = nullptr;
PFN_CreateSwapChainForComposition g_realCreateSwapChainForComposition = nullptr;
PFN_Present                       g_realPresent                       = nullptr;
PFN_Present1                      g_realPresent1                      = nullptr;
PFN_ResizeBuffers                 g_realResizeBuffers                 = nullptr;
PFN_ResizeBuffers1                g_realResizeBuffers1                = nullptr;

// ─── Captured Game D3D12 Objects ─────────────────────────────────────────────
std::mutex                  g_stateMutex;
ComPtr<IUnknown>            g_gameDeviceOrQueue;
ComPtr<ID3D12Device>        g_d3d12Device;
ComPtr<ID3D12CommandQueue>  g_d3d12Queue;

// ─── Distributed GPU Pipeline State ──────────────────────────────────────────
bool                                   g_hookInitialized = false;
dgpu::game_adapter::GameOffloadAdapter g_adapter;
dgpu::host::FrameCompositor            g_compositor;
dgpu::host::JobScheduler               g_scheduler;
dgpu::networking::TaskClient           g_workerClient;
dgpu::game_adapter::FramePacer         g_pacer;
bool                                   g_workerConnected = false;
static std::unique_ptr<dgpu::networking::WinsockInit> g_winsock;

bool g_enableWriteback   = false;
uint32_t g_downsampleFactor = 4; // Default 4x downsample (480x270 at 1080p, 518 KB payload)

// Resolves Worker Endpoint & Transport configuration from dgpu.ini, env, or registry
dgpu::networking::EndpointConfig resolveEndpointConfig() {
    dgpu::networking::EndpointConfig cfg;
    cfg.host = "";
    cfg.port = 7777;
    cfg.transport = dgpu::networking::TransportType::Auto;

    std::ifstream file("dgpu.ini");
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (line.empty() || line[0] == '#') continue;
            if (line.find("writeback=1") != std::string::npos) {
                g_enableWriteback = true;
                logMsg("[dgpu_dxgi] Writeback enabled from dgpu.ini");
            } else if (line.find("scale=2") != std::string::npos) {
                g_downsampleFactor = 2;
                logMsg("[dgpu_dxgi] Scale set to 2x (960x540) from dgpu.ini");
            } else if (line.starts_with("transport=")) {
                cfg.transport = dgpu::networking::stringToTransportType(line.substr(10));
                logMsg("[dgpu_dxgi] Loaded transport from dgpu.ini: " + line.substr(10));
            } else if (line.starts_with("interface=")) {
                cfg.interfaceName = line.substr(10);
                logMsg("[dgpu_dxgi] Loaded interface from dgpu.ini: " + cfg.interfaceName);
            } else if (line.starts_with("bind_ip=")) {
                cfg.localBindIp = line.substr(8);
                logMsg("[dgpu_dxgi] Loaded bind_ip from dgpu.ini: " + cfg.localBindIp);
            } else if (line.starts_with("usb4_ip=") && cfg.transport == dgpu::networking::TransportType::TcpUsb4) {
                cfg.host = line.substr(8);
                logMsg("[dgpu_dxgi] Loaded USB4 worker IP from dgpu.ini: " + cfg.host);
            } else if (cfg.host.empty() && line.find('=') == std::string::npos) {
                cfg.host = line;
                logMsg("[dgpu_dxgi] Loaded worker IP from dgpu.ini: " + line);
            }
        }
    }

    if (cfg.host.empty()) {
        char envIp[128] = {0};
        if (GetEnvironmentVariableA("DGPU_WORKER_IP", envIp, sizeof(envIp)) > 0 && envIp[0] != '\0') {
            cfg.host = envIp;
            logMsg("[dgpu_dxgi] Loaded worker IP from DGPU_WORKER_IP env: " + std::string(envIp));
        }
    }

    if (cfg.host.empty()) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char regVal[128] = {0};
            DWORD sz = sizeof(regVal);
            if (RegQueryValueExA(hKey, "DGPU_WORKER_IP", nullptr, nullptr, reinterpret_cast<LPBYTE>(regVal), &sz) == ERROR_SUCCESS && regVal[0] != '\0') {
                RegCloseKey(hKey);
                cfg.host = regVal;
                logMsg("[dgpu_dxgi] Loaded worker IP from HKCU\\Environment: " + std::string(regVal));
            } else {
                RegCloseKey(hKey);
            }
        }
    }

    if (cfg.host.empty()) {
        cfg.host = "127.0.0.1";
        logMsg("[dgpu_dxgi] Defaulting to localhost: 127.0.0.1");
    }

    return cfg;
}

std::string resolveWorkerIp() {
    return resolveEndpointConfig().host;
}

// ─── Swapchain Format & Color Space Handling ─────────────────────────────────
enum class PixelLayout {
    Rgba8,
    Bgra8,
    R10G10B10A2,
    R16G16B16A16F, // HDR10 / scRGB FP16
    Unsupported
};

struct FormatDetails {
    PixelLayout layout;
    uint32_t    bytesPerPixel;
};

FormatDetails inspectFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return { PixelLayout::Rgba8, 4 };
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return { PixelLayout::Bgra8, 4 };
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return { PixelLayout::R10G10B10A2, 4 };
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { PixelLayout::R16G16B16A16F, 8 };
    default:
        return { PixelLayout::Unsupported, 0 };
    }
}

inline float halfToFloat(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    int32_t  exp  = (h & 0x7C00) >> 10;
    uint32_t mant = (h & 0x03FF);

    if (exp == 0) {
        if (mant == 0) {
            uint32_t u = sign;
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
        while (!(mant & 0x0400)) {
            mant <<= 1;
            exp--;
        }
        exp++;
        mant &= ~0x0400;
    } else if (exp == 31) {
        uint32_t u = sign | 0x7F800000 | (mant << 13);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }

    exp = exp + (127 - 15);
    uint32_t u = sign | (static_cast<uint32_t>(exp) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

inline uint16_t floatToHalf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    uint32_t sign = (u >> 16) & 0x8000;
    int32_t  exp  = ((u >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (u & 0x007FFFFF) >> 13;

    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (exp << 10) | mant);
}

// ─── D3D12 Staging Resources for Backbuffer Capture & Writeback ──────────────
struct D3D12StagingResources {
    static constexpr uint32_t RING_SIZE = 4;
    ComPtr<ID3D12CommandAllocator>    cmdAllocs[RING_SIZE];
    ComPtr<ID3D12GraphicsCommandList> cmdLists[RING_SIZE];
    ComPtr<ID3D12Fence>               fence;
    HANDLE                            fenceEvent{nullptr};
    UINT64                            fenceValue{0};
    UINT64                            readbackFenceValues[RING_SIZE]{0, 0, 0, 0};
    ComPtr<ID3D12Resource>            readbackBuffers[RING_SIZE];
    uint32_t                          ringIdx{0};

    ComPtr<ID3D12Resource>            uploadBuffer;
    ComPtr<ID3D12CommandAllocator>    uploadAlloc;
    ComPtr<ID3D12GraphicsCommandList> uploadCmdList;
    ComPtr<ID3D12Fence>               uploadFence;
    HANDLE                            uploadFenceEvent{nullptr};
    UINT64                            uploadFenceValue{0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64                            totalSize{0};
    uint32_t                          width{0};
    uint32_t                          height{0};
    DXGI_FORMAT                       format{DXGI_FORMAT_UNKNOWN};
    bool                              valid{false};

    ~D3D12StagingResources() {
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        if (uploadFenceEvent) {
            CloseHandle(uploadFenceEvent);
            uploadFenceEvent = nullptr;
        }
    }

    bool ensure(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
        if (valid && width == w && height == h && format == fmt) {
            return true;
        }
        if (!device || !queue || w == 0 || h == 0) return false;

        valid  = false;
        width  = w;
        height = h;
        format = fmt;

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = w;
        texDesc.Height           = h;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = fmt;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        UINT numRows = 0;
        UINT64 rowSize = 0;
        device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalSize);

        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width            = totalSize;
        bufDesc.Height           = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels        = 1;
        bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (uint32_t i = 0; i < RING_SIZE; ++i) {
            readbackBuffers[i].Reset();
            if (FAILED(device->CreateCommittedResource(
                    &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&readbackBuffers[i])))) {
                logMsg("[dgpu_dxgi] Failed to allocate readback staging resource " + std::to_string(i));
                return false;
            }

            if (!cmdAllocs[i]) {
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocs[i]))))
                    return false;
            }
            if (!cmdLists[i]) {
                if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAllocs[i].Get(), nullptr, IID_PPV_ARGS(&cmdLists[i]))))
                    return false;
                cmdLists[i]->Close();
            }
        }

        if (!fence) {
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                return false;
        }
        if (!fenceEvent) {
            fenceEvent = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
        }
        fenceValue = 0;
        for (uint32_t i = 0; i < RING_SIZE; ++i) {
            readbackFenceValues[i] = 0;
        }

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadBuffer.Reset();
        if (FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadBuffer)))) {
            logMsg("[dgpu_dxgi] Failed to allocate upload staging resource");
            return false;
        }

        if (!uploadAlloc) {
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAlloc));
        }
        if (!uploadCmdList) {
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc.Get(), nullptr, IID_PPV_ARGS(&uploadCmdList));
            uploadCmdList->Close();
        }
        if (!uploadFence) {
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence));
        }
        if (!uploadFenceEvent) {
            uploadFenceEvent = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
        }
        uploadFenceValue = 0;

        valid = true;
        ringIdx = 0;
        logMsg("[dgpu_dxgi] D3D12 staging zero-stall ring resources successfully created for " + std::to_string(w) + "x" + std::to_string(h));
        return true;
    }
};

static D3D12StagingResources g_staging;

// ─── Asynchronous Pipeline Ring-State (Non-Blocking N-1 Offload) ─────────────
struct InFlightFrameState {
    bool                                  hasJob{false};
    dgpu::task_runtime::JobHandle         jobHandle;
    std::vector<std::byte>                compositedOutput;
    uint32_t                              offloadW{0};
    uint32_t                              offloadH{0};
    UINT64                                captureFenceVal{0};
    std::chrono::steady_clock::time_point submitTime{std::chrono::steady_clock::now()};
};

static std::deque<InFlightFrameState> g_inFlightQueue;

// ─── Forward Declaration of Hook Installers ──────────────────────────────────
void installSwapChainHook(IDXGISwapChain* pSwapChain, IUnknown* pDevice);
void installFactoryHook(IDXGIFactory* pFactory);

void initSystemDxgi() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        wchar_t systemPath[MAX_PATH];
        UINT len = GetSystemDirectoryW(systemPath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH - 16) {
            wcscpy_s(systemPath, L"C:\\Windows\\System32");
        }
        wcscat_s(systemPath, L"\\dxgi.dll");

        g_systemDxgiModule = LoadLibraryW(systemPath);
        if (!g_systemDxgiModule) {
            logMsg("[dgpu_dxgi] CRITICAL: Failed to load system dxgi.dll");
            return;
        }

        g_realCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(
            GetProcAddress(g_systemDxgiModule, "CreateDXGIFactory"));
        g_realCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
            GetProcAddress(g_systemDxgiModule, "CreateDXGIFactory1"));
        g_realCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
            GetProcAddress(g_systemDxgiModule, "CreateDXGIFactory2"));
        g_realDXGIDeclareAdapterRemovalSupport = reinterpret_cast<PFN_DXGIDeclareAdapterRemovalSupport>(
            GetProcAddress(g_systemDxgiModule, "DXGIDeclareAdapterRemovalSupport"));
        g_realDXGIGetDebugInterface1 = reinterpret_cast<PFN_DXGIGetDebugInterface1>(
            GetProcAddress(g_systemDxgiModule, "DXGIGetDebugInterface1"));

        logMsg("[dgpu_dxgi] System DXGI successfully loaded and forwarders bound.");
    });
}

void initDistributedHook(IDXGISwapChain* swapChain) {
    if (g_hookInitialized) return;
    std::lock_guard<std::mutex> lk(g_stateMutex);
    if (g_hookInitialized) return;

    if (!g_winsock) {
        g_winsock = std::make_unique<dgpu::networking::WinsockInit>();
        if (!g_winsock->ok()) {
            logMsg("[dgpu_dxgi] CRITICAL: Winsock initialization failed");
            return;
        }
        logMsg("[dgpu_dxgi] Winsock initialized successfully.");
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return;

    auto endpoint = resolveEndpointConfig();

    logMsg("[dgpu_dxgi] Connecting to Distributed GPU worker (" +
           std::string(dgpu::networking::transportTypeToString(endpoint.transport)) + ") at " +
           endpoint.host + ":" + std::to_string(endpoint.port) + "...");
    g_workerConnected = g_workerClient.connect(endpoint);
    if (g_workerConnected) {
        logMsg("[dgpu_dxgi] Connected to Distributed GPU worker. Handshake verified.");
    } else {
        logMsg("[dgpu_dxgi] Worker unreachable; local fallback active.");
    }

    g_scheduler.setRemoteExecutor([](const dgpu::protocol::TaskRequest& req) {
        if (!g_workerClient.isConnected()) {
            auto ep = resolveEndpointConfig();
            g_workerClient.connect(ep);
        }
        return g_workerClient.submit(req);
    });
    g_scheduler.setLocalExecutor([](const dgpu::protocol::TaskRequest& req) {
        dgpu::protocol::TaskResult res;
        res.taskId = req.taskId;
        res.status = dgpu::protocol::TaskStatus::Ok;
        res.output = req.input;
        return res;
    });
    g_scheduler.start(1);

    uint32_t offloadW = desc.BufferDesc.Width  > 0 ? desc.BufferDesc.Width / 2  : 960;
    uint32_t offloadH = desc.BufferDesc.Height > 0 ? desc.BufferDesc.Height / 2 : 540;
    g_adapter.init(nullptr, offloadW, offloadH);

    g_hookInitialized = true;
    logMsg("[dgpu_dxgi] Distributed GPU pipeline initialized on SwapChain.");
}

// ─── Asynchronous D3D12 Capture & Pipelined Composition ──────────────────────
void processPipelinedFrame(IDXGISwapChain* pSwapChain) {
    if (!g_hookInitialized) return;

    // 1. Query active backbuffer
    ComPtr<IDXGISwapChain3> sc3;
    pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
    UINT bbIndex = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;

    ComPtr<ID3D12Resource> backBuffer;
    HRESULT hrBuf = pSwapChain->GetBuffer(bbIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hrBuf) || !backBuffer) {
        static uint32_t s_bufFailCount = 0;
        if (++s_bufFailCount % 120 == 1) {
            logMsg("[dgpu_dxgi] WARNING: GetBuffer(" + std::to_string(bbIndex) + ") failed: hr=0x" + std::to_string(hrBuf));
        }
        return;
    }

    D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    uint32_t width  = static_cast<uint32_t>(desc.Width);
    uint32_t height = desc.Height;
    if (width == 0 || height == 0) return;

    // 2. Resolve Device & Command Queue (refresh if changed on SwapChain)
    ComPtr<ID3D12CommandQueue> curQueue;
    if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&curQueue))) && curQueue) {
        if (g_d3d12Queue.Get() != curQueue.Get()) {
            std::lock_guard<std::mutex> lk(g_stateMutex);
            g_d3d12Queue = curQueue;
            curQueue->GetDevice(IID_PPV_ARGS(&g_d3d12Device));
            logMsg("[dgpu_dxgi] Bound active command queue from swapchain");
        }
    }

    ID3D12Device*       device = g_d3d12Device.Get();
    ID3D12CommandQueue* queue  = g_d3d12Queue.Get();

    if (!device) {
        backBuffer->GetDevice(IID_PPV_ARGS(&g_d3d12Device));
        device = g_d3d12Device.Get();
    }
    if (!device) {
        pSwapChain->GetDevice(IID_PPV_ARGS(&g_d3d12Device));
        device = g_d3d12Device.Get();
    }
    if (!queue && device) {
        pSwapChain->GetDevice(IID_PPV_ARGS(&g_d3d12Queue));
        queue = g_d3d12Queue.Get();
        if (!queue) {
            // Create a dedicated direct command queue for staging copies
            D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
            if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_d3d12Queue)))) {
                queue = g_d3d12Queue.Get();
                logMsg("[dgpu_dxgi] Created dedicated D3D12 staging queue on device");
            }
        }
    }
    if (!device || !queue) {
        static uint32_t s_devFailCount = 0;
        if (++s_devFailCount % 120 == 1) {
            logMsg("[dgpu_dxgi] WARNING: Missing D3D12 device or queue");
        }
        return;
    }

    FormatDetails fmtDetails = inspectFormat(desc.Format);
    if (fmtDetails.layout == PixelLayout::Unsupported) {
        static DXGI_FORMAT s_lastBadFmt = DXGI_FORMAT_UNKNOWN;
        if (desc.Format != s_lastBadFmt) {
            s_lastBadFmt = desc.Format;
            logMsg("[dgpu_dxgi] WARNING: Unsupported backbuffer format: " + std::to_string(static_cast<int>(desc.Format)));
        }
        return;
    }

    static bool s_loggedFirstFrame = false;
    if (!s_loggedFirstFrame) {
        s_loggedFirstFrame = true;
        logMsg("[dgpu_dxgi] First frame captured: " + std::to_string(width) + "x" + std::to_string(height) +
               ", format=" + std::to_string(desc.Format));
    }

    // 3. Check previous in-flight tasks (Double-Buffered N-1 / N-2 Pipeline)
    while (!g_inFlightQueue.empty()) {
        auto& front = g_inFlightQueue.front();
        if (front.jobHandle.future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto result = front.jobHandle.future.get();
            uint32_t offloadW = front.offloadW;
            uint32_t offloadH = front.offloadH;
            UINT64 captureFenceVal = front.captureFenceVal;
            g_inFlightQueue.pop_front();

            if (result.status == dgpu::protocol::TaskStatus::Ok && !result.output.empty()) {
                g_scheduler.recordRemoteSuccess(result.gpuUs, result.totalUs > result.gpuUs ? result.totalUs - result.gpuUs : 0);
                static uint32_t s_recvCount = 0;
                if (++s_recvCount % 60 == 1) {
                    logMsg("[dgpu_dxgi] Completed remote offload frame " + std::to_string(s_recvCount) +
                           " from worker (GPU=" + std::to_string(result.gpuUs) + "us, Total=" +
                           std::to_string(result.totalUs) + "us, queue=" + std::to_string(g_inFlightQueue.size()) + ")");
                }

                if (g_enableWriteback && g_staging.fence->GetCompletedValue() >= captureFenceVal) {
                    void* mapped = nullptr;
                    D3D12_RANGE writeRange{ 0, static_cast<SIZE_T>(g_staging.totalSize) };
                    if (SUCCEEDED(g_staging.uploadBuffer->Map(0, &writeRange, &mapped)) && mapped) {
                        uint8_t* dst = static_cast<uint8_t*>(mapped);
                        UINT rowPitch = g_staging.footprint.Footprint.RowPitch;
                        const uint8_t* comp = reinterpret_cast<const uint8_t*>(result.output.data());

                        if (fmtDetails.layout == PixelLayout::R16G16B16A16F) {
                            for (uint32_t y = 0; y < height; ++y) {
                                auto* dstHalf = reinterpret_cast<uint16_t*>(dst + y * rowPitch);
                                for (uint32_t x = 0; x < width; ++x) {
                                    uint32_t sx = (x * offloadW) / width;
                                    uint32_t sy = (y * offloadH) / height;
                                    size_t cIdx = (static_cast<size_t>(sy) * offloadW + sx) * 4;
                                    float bloomR = comp[cIdx + 0] / 255.0f;
                                    float bloomG = comp[cIdx + 1] / 255.0f;
                                    float bloomB = comp[cIdx + 2] / 255.0f;

                                    float r = halfToFloat(dstHalf[x * 4 + 0]) + bloomR * 0.3f;
                                    float g = halfToFloat(dstHalf[x * 4 + 1]) + bloomG * 0.3f;
                                    float b = halfToFloat(dstHalf[x * 4 + 2]) + bloomB * 0.3f;
                                    dstHalf[x * 4 + 0] = floatToHalf(r);
                                    dstHalf[x * 4 + 1] = floatToHalf(g);
                                    dstHalf[x * 4 + 2] = floatToHalf(b);
                                }
                            }
                        } else if (fmtDetails.layout == PixelLayout::R10G10B10A2) {
                            // Elden Ring 10-bit format: blend bloom pass into backbuffer
                            for (uint32_t y = 0; y < height; ++y) {
                                auto* dst32 = reinterpret_cast<uint32_t*>(dst + y * rowPitch);
                                for (uint32_t x = 0; x < width; ++x) {
                                    uint32_t sx = (x * offloadW) / width;
                                    uint32_t sy = (y * offloadH) / height;
                                    size_t cIdx = (static_cast<size_t>(sy) * offloadW + sx) * 4;
                                    uint32_t orig = dst32[x];
                                    uint32_t r = orig & 0x3FF;
                                    uint32_t g = (orig >> 10) & 0x3FF;
                                    uint32_t b = (orig >> 20) & 0x3FF;
                                    uint32_t a = (orig >> 30) & 0x3;
                                    r = std::min(1023u, r + static_cast<uint32_t>(comp[cIdx + 0] * 1023 / 255 * 0.15f));
                                    g = std::min(1023u, g + static_cast<uint32_t>(comp[cIdx + 1] * 1023 / 255 * 0.15f));
                                    b = std::min(1023u, b + static_cast<uint32_t>(comp[cIdx + 2] * 1023 / 255 * 0.15f));
                                    dst32[x] = r | (g << 10) | (b << 20) | (a << 30);
                                }
                            }
                        } else if (fmtDetails.layout == PixelLayout::Bgra8) {
                            for (uint32_t y = 0; y < height; ++y) {
                                auto* dst8 = dst + y * rowPitch;
                                for (uint32_t x = 0; x < width; ++x) {
                                    uint32_t sx = (x * offloadW) / width;
                                    uint32_t sy = (y * offloadH) / height;
                                    size_t cIdx = (static_cast<size_t>(sy) * offloadW + sx) * 4;
                                    size_t dIdx = x * 4;
                                    dst8[dIdx + 0] = static_cast<uint8_t>(std::min(255, dst8[dIdx + 0] + static_cast<int>(comp[cIdx + 2] * 0.2f)));
                                    dst8[dIdx + 1] = static_cast<uint8_t>(std::min(255, dst8[dIdx + 1] + static_cast<int>(comp[cIdx + 1] * 0.2f)));
                                    dst8[dIdx + 2] = static_cast<uint8_t>(std::min(255, dst8[dIdx + 2] + static_cast<int>(comp[cIdx + 0] * 0.2f)));
                                }
                            }
                        } else {
                            for (uint32_t y = 0; y < height; ++y) {
                                auto* dst8 = dst + y * rowPitch;
                                for (uint32_t x = 0; x < width; ++x) {
                                    uint32_t sx = (x * offloadW) / width;
                                    uint32_t sy = (y * offloadH) / height;
                                    size_t cIdx = (static_cast<size_t>(sy) * offloadW + sx) * 4;
                                    size_t dIdx = x * 4;
                                    dst8[dIdx + 0] = static_cast<uint8_t>(std::min(255, dst8[dIdx + 0] + static_cast<int>(comp[cIdx + 0] * 0.2f)));
                                    dst8[dIdx + 1] = static_cast<uint8_t>(std::min(255, dst8[dIdx + 1] + static_cast<int>(comp[cIdx + 1] * 0.2f)));
                                    dst8[dIdx + 2] = static_cast<uint8_t>(std::min(255, dst8[dIdx + 2] + static_cast<int>(comp[cIdx + 2] * 0.2f)));
                                }
                            }
                        }
                        g_staging.uploadBuffer->Unmap(0, &writeRange);

                        // Ensure previous upload on GPU has finished before resetting upload allocator
                        if (g_staging.uploadFence && g_staging.uploadFenceValue > 0 &&
                            g_staging.uploadFence->GetCompletedValue() < g_staging.uploadFenceValue) {
                            g_staging.uploadFence->SetEventOnCompletion(g_staging.uploadFenceValue, g_staging.uploadFenceEvent);
                            WaitForSingleObject(g_staging.uploadFenceEvent, 50);
                        }

                        g_staging.uploadAlloc->Reset();
                        g_staging.uploadCmdList->Reset(g_staging.uploadAlloc.Get(), nullptr);

                        D3D12_RESOURCE_BARRIER toDest{};
                        toDest.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        toDest.Transition.pResource   = backBuffer.Get();
                        toDest.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                        toDest.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                        toDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        g_staging.uploadCmdList->ResourceBarrier(1, &toDest);

                        D3D12_TEXTURE_COPY_LOCATION uploadSrc{};
                        uploadSrc.pResource       = g_staging.uploadBuffer.Get();
                        uploadSrc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        uploadSrc.PlacedFootprint = g_staging.footprint;

                        D3D12_TEXTURE_COPY_LOCATION bbDst{};
                        bbDst.pResource        = backBuffer.Get();
                        bbDst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        bbDst.SubresourceIndex = 0;

                        g_staging.uploadCmdList->CopyTextureRegion(&bbDst, 0, 0, 0, &uploadSrc, nullptr);

                        D3D12_RESOURCE_BARRIER toPres{};
                        toPres.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        toPres.Transition.pResource   = backBuffer.Get();
                        toPres.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                        toPres.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                        toPres.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        g_staging.uploadCmdList->ResourceBarrier(1, &toPres);

                        g_staging.uploadCmdList->Close();
                        ID3D12CommandList* lists[] = { g_staging.uploadCmdList.Get() };
                        queue->ExecuteCommandLists(1, lists);

                        UINT64 upFence = ++g_staging.uploadFenceValue;
                        queue->Signal(g_staging.uploadFence.Get(), upFence);
                    }
                }
            }
        } else {
            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - front.submitTime).count();
            if (elapsedMs > 3000) {
                logMsg("[dgpu_dxgi] WARNING: In-flight task timed out (" + std::to_string(elapsedMs) + "ms); resetting connection.");
                g_workerClient.disconnect();
                g_inFlightQueue.pop_front();
            }
            break;
        }
    }

    // Multi-frame pipeline: allow up to 4 frames in flight to absorb network fluctuations and eliminate GPU idle bubbles
    if (g_inFlightQueue.size() >= 4) {
        return;
    }

    // 4. Ensure staging buffers for current frame
    if (!g_staging.ensure(device, queue, width, height, desc.Format)) {
        static uint32_t s_stagingFail = 0;
        if (++s_stagingFail % 120 == 1) {
            logMsg("[dgpu_dxgi] WARNING: g_staging.ensure failed for " + std::to_string(width) + "x" + std::to_string(height));
        }
        return;
    }

    // 5. Asynchronous zero-stall ring buffer copy
    uint32_t writeIdx = g_staging.ringIdx;
    uint32_t readIdx  = (g_staging.ringIdx + 1) % D3D12StagingResources::RING_SIZE;
    g_staging.ringIdx = (g_staging.ringIdx + 1) % D3D12StagingResources::RING_SIZE;

    // Verify that the GPU has finished executing commands from the previous use of this ring slot
    if (g_staging.readbackFenceValues[writeIdx] > 0 &&
        g_staging.fence->GetCompletedValue() < g_staging.readbackFenceValues[writeIdx]) {
        g_staging.fence->SetEventOnCompletion(g_staging.readbackFenceValues[writeIdx], g_staging.fenceEvent);
        WaitForSingleObject(g_staging.fenceEvent, 50);
    }

    g_staging.cmdAllocs[writeIdx]->Reset();
    g_staging.cmdLists[writeIdx]->Reset(g_staging.cmdAllocs[writeIdx].Get(), nullptr);

    D3D12_RESOURCE_BARRIER toSrc{};
    toSrc.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrc.Transition.pResource   = backBuffer.Get();
    toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toSrc.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_staging.cmdLists[writeIdx]->ResourceBarrier(1, &toSrc);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource       = g_staging.readbackBuffers[writeIdx].Get();
    dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = g_staging.footprint;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = backBuffer.Get();
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    g_staging.cmdLists[writeIdx]->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER backToPres{};
    backToPres.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backToPres.Transition.pResource   = backBuffer.Get();
    backToPres.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    backToPres.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    backToPres.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_staging.cmdLists[writeIdx]->ResourceBarrier(1, &backToPres);

    g_staging.cmdLists[writeIdx]->Close();
    ID3D12CommandList* copyLists[] = { g_staging.cmdLists[writeIdx].Get() };
    queue->ExecuteCommandLists(1, copyLists);

    UINT64 currentFence = ++g_staging.fenceValue;
    g_staging.readbackFenceValues[writeIdx] = currentFence;
    queue->Signal(g_staging.fence.Get(), currentFence);

    // 6. Zero-stall extraction from buffer submitted frames ago (readIdx)
    if (g_staging.readbackFenceValues[readIdx] == 0) {
        // Initial warm-up frames: ring buffer has not yet circled
        return;
    }

    if (g_staging.fence->GetCompletedValue() >= g_staging.readbackFenceValues[readIdx]) {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(g_staging.totalSize) };
        if (SUCCEEDED(g_staging.readbackBuffers[readIdx]->Map(0, &readRange, &mapped)) && mapped) {
            uint32_t offloadW = width / g_downsampleFactor;
            uint32_t offloadH = height / g_downsampleFactor;
            static std::vector<std::byte> s_downsampleBuf;
            size_t reqBytes = static_cast<size_t>(offloadW) * offloadH * 4;
            if (s_downsampleBuf.size() != reqBytes) {
                s_downsampleBuf.resize(reqBytes);
            }

            const uint8_t* src = static_cast<const uint8_t*>(mapped);
            UINT rowPitch = g_staging.footprint.Footprint.RowPitch;
            const uint32_t step = g_downsampleFactor;

            alignas(64) uint32_t localRow[2048];
            const size_t rowBytes = std::min<size_t>(sizeof(localRow), static_cast<size_t>(width) * 4);

            if (fmtDetails.layout == PixelLayout::R16G16B16A16F) {
                for (uint32_t y = 0; y < offloadH; ++y) {
                    std::memcpy(localRow, src + (y * step) * rowPitch, rowBytes);
                    const auto* srcRow = reinterpret_cast<const uint16_t*>(localRow);
                    size_t dIdx = static_cast<size_t>(y) * offloadW * 4;
                    for (uint32_t x = 0; x < offloadW; ++x) {
                        float r = halfToFloat(srcRow[(x * step) * 4 + 0]);
                        float g = halfToFloat(srcRow[(x * step) * 4 + 1]);
                        float b = halfToFloat(srcRow[(x * step) * 4 + 2]);
                        s_downsampleBuf[dIdx + 0] = static_cast<std::byte>(std::clamp(r * 255.0f, 0.0f, 255.0f));
                        s_downsampleBuf[dIdx + 1] = static_cast<std::byte>(std::clamp(g * 255.0f, 0.0f, 255.0f));
                        s_downsampleBuf[dIdx + 2] = static_cast<std::byte>(std::clamp(b * 255.0f, 0.0f, 255.0f));
                        s_downsampleBuf[dIdx + 3] = static_cast<std::byte>(255);
                        dIdx += 4;
                    }
                }
            } else if (fmtDetails.layout == PixelLayout::R10G10B10A2) {
                // Elden Ring 10-bit format: unpack 10:10:10:2 to RGBA8 via burst cache line copy
                for (uint32_t y = 0; y < offloadH; ++y) {
                    std::memcpy(localRow, src + (y * step) * rowPitch, rowBytes);
                    size_t dIdx = static_cast<size_t>(y) * offloadW * 4;
                    for (uint32_t x = 0; x < offloadW; ++x) {
                        uint32_t p = localRow[x * step];
                        uint8_t r = static_cast<uint8_t>((p & 0x3FF) >> 2);
                        uint8_t g = static_cast<uint8_t>(((p >> 10) & 0x3FF) >> 2);
                        uint8_t b = static_cast<uint8_t>(((p >> 20) & 0x3FF) >> 2);
                        s_downsampleBuf[dIdx + 0] = static_cast<std::byte>(r);
                        s_downsampleBuf[dIdx + 1] = static_cast<std::byte>(g);
                        s_downsampleBuf[dIdx + 2] = static_cast<std::byte>(b);
                        s_downsampleBuf[dIdx + 3] = static_cast<std::byte>(255);
                        dIdx += 4;
                    }
                }
            } else if (fmtDetails.layout == PixelLayout::Bgra8) {
                for (uint32_t y = 0; y < offloadH; ++y) {
                    std::memcpy(localRow, src + (y * step) * rowPitch, rowBytes);
                    const auto* srcRow = reinterpret_cast<const uint8_t*>(localRow);
                    size_t dIdx = static_cast<size_t>(y) * offloadW * 4;
                    for (uint32_t x = 0; x < offloadW; ++x) {
                        size_t sIdx = (x * step) * 4;
                        s_downsampleBuf[dIdx + 0] = static_cast<std::byte>(srcRow[sIdx + 2]);
                        s_downsampleBuf[dIdx + 1] = static_cast<std::byte>(srcRow[sIdx + 1]);
                        s_downsampleBuf[dIdx + 2] = static_cast<std::byte>(srcRow[sIdx + 0]);
                        s_downsampleBuf[dIdx + 3] = static_cast<std::byte>(srcRow[sIdx + 3]);
                        dIdx += 4;
                    }
                }
            } else {
                for (uint32_t y = 0; y < offloadH; ++y) {
                    std::memcpy(localRow, src + (y * step) * rowPitch, rowBytes);
                    const auto* srcRow = reinterpret_cast<const uint8_t*>(localRow);
                    size_t dIdx = static_cast<size_t>(y) * offloadW * 4;
                    for (uint32_t x = 0; x < offloadW; ++x) {
                        size_t sIdx = (x * step) * 4;
                        std::memcpy(&s_downsampleBuf[dIdx], &srcRow[sIdx], 4);
                        dIdx += 4;
                    }
                }
            }
            D3D12_RANGE emptyRange{0, 0};
            g_staging.readbackBuffers[readIdx]->Unmap(0, &emptyRange);

            static uint32_t s_submittedCount = 0;
            if (++s_submittedCount % 60 == 1) {
                logMsg("[dgpu_dxgi] Submitting frame " + std::to_string(s_submittedCount) + " (" +
                       std::to_string(offloadW) + "x" + std::to_string(offloadH) + ") to worker");
            }

            dgpu::protocol::TaskRequest req;
            req.taskId = 8000 + currentFence;
            req.type   = dgpu::protocol::TaskType::Blur;
            req.width  = offloadW;
            req.height = offloadH;
            req.input  = s_downsampleBuf;

            InFlightFrameState job;
            job.jobHandle       = g_scheduler.submit(std::move(req), dgpu::task_runtime::ExecutionSite::Remote);
            job.hasJob          = true;
            job.offloadW        = offloadW;
            job.offloadH        = offloadH;
            job.captureFenceVal = currentFence;
            job.submitTime      = std::chrono::steady_clock::now();
            g_inFlightQueue.push_back(std::move(job));
        }
    } else {
        static uint32_t s_fenceDrop = 0;
        if (++s_fenceDrop % 60 == 1) {
            logMsg("[dgpu_dxgi] WARNING: Staging fence wait timed out (completed=" +
                   std::to_string(g_staging.fence->GetCompletedValue()) + " < target=" +
                   std::to_string(g_staging.readbackFenceValues[readIdx]) + ")");
        }
    }
}

// ─── Hooked SwapChain Methods ────────────────────────────────────────────────
HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* This,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags) {

    logMsg("[dgpu_dxgi] Intercepted IDXGISwapChain::ResizeBuffers (" +
           std::to_string(Width) + "x" + std::to_string(Height) + ", fmt=" + std::to_string(NewFormat) + ")");
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_staging.valid = false;
        g_inFlightQueue.clear();
    }
    if (!g_realResizeBuffers) return E_FAIL;
    return g_realResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(
    IDXGISwapChain3* This,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags,
    const UINT* pCreationNodeMask,
    IUnknown* const* ppPresentQueue) {

    logMsg("[dgpu_dxgi] Intercepted IDXGISwapChain3::ResizeBuffers1 (" +
           std::to_string(Width) + "x" + std::to_string(Height) + ", fmt=" + std::to_string(NewFormat) + ")");
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_staging.valid = false;
        g_inFlightQueue.clear();
    }
    if (!g_realResizeBuffers1) return E_FAIL;
    return g_realResizeBuffers1(This, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    auto tStart = std::chrono::steady_clock::now();

    static uint64_t s_presentCalls = 0;
    uint64_t curCall = ++s_presentCalls;
    if (curCall % 120 == 1) {
        logMsg("[dgpu_dxgi] Present heartbeat #" + std::to_string(curCall) + " (SyncInterval=" + std::to_string(SyncInterval) + ")");
    }

    static IDXGISwapChain* s_lastSwapChain = nullptr;
    if (pSwapChain != s_lastSwapChain) {
        s_lastSwapChain = pSwapChain;
        logMsg("[dgpu_dxgi] SwapChain pointer active in HookedPresent");
        installSwapChainHook(pSwapChain, nullptr);
    }

    if (!g_hookInitialized) {
        initDistributedHook(pSwapChain);
    }

    processPipelinedFrame(pSwapChain);

    HRESULT hr = g_realPresent ? g_realPresent(pSwapChain, SyncInterval, Flags) : S_OK;

    auto tEnd = std::chrono::steady_clock::now();
    uint64_t frameUs = std::chrono::duration_cast<std::chrono::microseconds>(tEnd - tStart).count();
    g_pacer.recordFrame(frameUs);

    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    auto tStart = std::chrono::steady_clock::now();

    static uint64_t s_present1Calls = 0;
    uint64_t curCall = ++s_present1Calls;
    if (curCall % 120 == 1) {
        logMsg("[dgpu_dxgi] Present1 heartbeat #" + std::to_string(curCall) + " (SyncInterval=" + std::to_string(SyncInterval) + ")");
    }

    static IDXGISwapChain1* s_lastSwapChain1 = nullptr;
    if (pSwapChain != s_lastSwapChain1) {
        s_lastSwapChain1 = pSwapChain;
        logMsg("[dgpu_dxgi] SwapChain1 pointer active in HookedPresent1");
        installSwapChainHook(pSwapChain, nullptr);
    }

    if (!g_hookInitialized) {
        initDistributedHook(pSwapChain);
    }

    processPipelinedFrame(pSwapChain);

    HRESULT hr = S_OK;
    if (g_realPresent1) {
        hr = g_realPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    } else if (g_realPresent) {
        hr = g_realPresent(pSwapChain, SyncInterval, Flags);
    }

    auto tEnd = std::chrono::steady_clock::now();
    uint64_t frameUs = std::chrono::duration_cast<std::chrono::microseconds>(tEnd - tStart).count();
    g_pacer.recordFrame(frameUs);

    return hr;
}

// ─── SwapChain Hook Installer ────────────────────────────────────────────────
void installSwapChainHook(IDXGISwapChain* pSwapChain, IUnknown* pDevice) {
    if (!pSwapChain) return;

    if (pDevice) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_gameDeviceOrQueue = pDevice;
        pDevice->QueryInterface(IID_PPV_ARGS(&g_d3d12Queue));
        if (g_d3d12Queue) {
            g_d3d12Queue->GetDevice(IID_PPV_ARGS(&g_d3d12Device));
            logMsg("[dgpu_dxgi] Captured D3D12 CommandQueue and Device from SwapChain");
        } else {
            pDevice->QueryInterface(IID_PPV_ARGS(&g_d3d12Device));
            logMsg("[dgpu_dxgi] Captured D3D12 Device from SwapChain");
        }
    }

    void** vtable = *reinterpret_cast<void***>(pSwapChain);
    DWORD oldProtect = 0;

    // Hook IDXGISwapChain::Present (index 8)
    if (vtable[8] != reinterpret_cast<void*>(&HookedPresent)) {
        if (VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (g_realPresent == nullptr) {
                g_realPresent = reinterpret_cast<PFN_Present>(vtable[8]);
            }
            vtable[8]     = reinterpret_cast<void*>(&HookedPresent);
            VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
            logMsg("[dgpu_dxgi] Intercepted IDXGISwapChain::Present");
        }
    }

    // Hook IDXGISwapChain::ResizeBuffers (index 13)
    if (vtable[13] != reinterpret_cast<void*>(&HookedResizeBuffers)) {
        if (VirtualProtect(&vtable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (g_realResizeBuffers == nullptr) {
                g_realResizeBuffers = reinterpret_cast<PFN_ResizeBuffers>(vtable[13]);
            }
            vtable[13] = reinterpret_cast<void*>(&HookedResizeBuffers);
            VirtualProtect(&vtable[13], sizeof(void*), oldProtect, &oldProtect);
            logMsg("[dgpu_dxgi] Intercepted IDXGISwapChain::ResizeBuffers");
        }
    }

    // Hook IDXGISwapChain1::Present1 (index 22) if supported
    ComPtr<IDXGISwapChain1> sc1;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        void** sc1Vtable = *reinterpret_cast<void***>(sc1.Get());
        if (sc1Vtable[22] != reinterpret_cast<void*>(&HookedPresent1)) {
            if (VirtualProtect(&sc1Vtable[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                if (g_realPresent1 == nullptr) {
                    g_realPresent1 = reinterpret_cast<PFN_Present1>(sc1Vtable[22]);
                }
                sc1Vtable[22]  = reinterpret_cast<void*>(&HookedPresent1);
                VirtualProtect(&sc1Vtable[22], sizeof(void*), oldProtect, &oldProtect);
                logMsg("[dgpu_dxgi] Intercepted IDXGISwapChain1::Present1");
            }
        }
    }

    // Hook IDXGISwapChain3::ResizeBuffers1 (index 39) if supported
    ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        void** sc3Vtable = *reinterpret_cast<void***>(sc3.Get());
        if (sc3Vtable[39] != reinterpret_cast<void*>(&HookedResizeBuffers1)) {
            if (VirtualProtect(&sc3Vtable[39], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                if (g_realResizeBuffers1 == nullptr) {
                    g_realResizeBuffers1 = reinterpret_cast<PFN_ResizeBuffers1>(sc3Vtable[39]);
                }
                sc3Vtable[39] = reinterpret_cast<void*>(&HookedResizeBuffers1);
                VirtualProtect(&sc3Vtable[39], sizeof(void*), oldProtect, &oldProtect);
                logMsg("[dgpu_dxgi] Intercepted IDXGISwapChain3::ResizeBuffers1");
            }
        }
    }
}

// ─── Hooked Factory Methods ──────────────────────────────────────────────────
HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory* This,
    IUnknown* pDevice,
    DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain) {

    logMsg("[dgpu_dxgi] Intercepted IDXGIFactory::CreateSwapChain");
    if (!g_realCreateSwapChain) return E_FAIL;

    HRESULT hr = g_realCreateSwapChain(This, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        installSwapChainHook(*ppSwapChain, pDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {

    logMsg("[dgpu_dxgi] Intercepted IDXGIFactory2::CreateSwapChainForHwnd");
    if (!g_realCreateSwapChainForHwnd) return E_FAIL;

    HRESULT hr = g_realCreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        installSwapChainHook(*ppSwapChain, pDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {

    logMsg("[dgpu_dxgi] Intercepted IDXGIFactory2::CreateSwapChainForCoreWindow");
    if (!g_realCreateSwapChainForCoreWindow) return E_FAIL;

    HRESULT hr = g_realCreateSwapChainForCoreWindow(This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        installSwapChainHook(*ppSwapChain, pDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForComposition(
    IDXGIFactory2* This,
    IUnknown* pDevice,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {

    logMsg("[dgpu_dxgi] Intercepted IDXGIFactory2::CreateSwapChainForComposition");
    if (!g_realCreateSwapChainForComposition) return E_FAIL;

    HRESULT hr = g_realCreateSwapChainForComposition(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        installSwapChainHook(*ppSwapChain, pDevice);
    }
    return hr;
}

// ─── Factory Hook Installer ──────────────────────────────────────────────────
void installFactoryHook(IDXGIFactory* pFactory) {
    if (!pFactory) return;

    void** vtable = *reinterpret_cast<void***>(pFactory);
    DWORD oldProtect = 0;

    // Hook IDXGIFactory::CreateSwapChain (index 10)
    if (vtable[10] != reinterpret_cast<void*>(&HookedCreateSwapChain)) {
        if (VirtualProtect(&vtable[10], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (g_realCreateSwapChain == nullptr) {
                g_realCreateSwapChain = reinterpret_cast<PFN_CreateSwapChain>(vtable[10]);
            }
            vtable[10]            = reinterpret_cast<void*>(&HookedCreateSwapChain);
            VirtualProtect(&vtable[10], sizeof(void*), oldProtect, &oldProtect);
            logMsg("[dgpu_dxgi] Intercepted IDXGIFactory::CreateSwapChain vtable");
        }
    }

    // Hook IDXGIFactory2 methods if available
    ComPtr<IDXGIFactory2> f2;
    if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&f2)))) {
        void** f2Vtable = *reinterpret_cast<void***>(f2.Get());
        // Hook CreateSwapChainForHwnd (index 15)
        if (f2Vtable[15] != reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd)) {
            if (VirtualProtect(&f2Vtable[15], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                if (g_realCreateSwapChainForHwnd == nullptr) {
                    g_realCreateSwapChainForHwnd = reinterpret_cast<PFN_CreateSwapChainForHwnd>(f2Vtable[15]);
                }
                f2Vtable[15]                 = reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd);
                VirtualProtect(&f2Vtable[15], sizeof(void*), oldProtect, &oldProtect);
                logMsg("[dgpu_dxgi] Intercepted IDXGIFactory2::CreateSwapChainForHwnd vtable");
            }
        }
        // Hook CreateSwapChainForCoreWindow (index 16)
        if (f2Vtable[16] != reinterpret_cast<void*>(&HookedCreateSwapChainForCoreWindow)) {
            if (VirtualProtect(&f2Vtable[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                if (g_realCreateSwapChainForCoreWindow == nullptr) {
                    g_realCreateSwapChainForCoreWindow = reinterpret_cast<PFN_CreateSwapChainForCoreWindow>(f2Vtable[16]);
                }
                f2Vtable[16]                       = reinterpret_cast<void*>(&HookedCreateSwapChainForCoreWindow);
                VirtualProtect(&f2Vtable[16], sizeof(void*), oldProtect, &oldProtect);
                logMsg("[dgpu_dxgi] Intercepted IDXGIFactory2::CreateSwapChainForCoreWindow vtable");
            }
        }
        // Hook CreateSwapChainForComposition (index 24)
        if (f2Vtable[24] != reinterpret_cast<void*>(&HookedCreateSwapChainForComposition)) {
            if (VirtualProtect(&f2Vtable[24], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                if (g_realCreateSwapChainForComposition == nullptr) {
                    g_realCreateSwapChainForComposition = reinterpret_cast<PFN_CreateSwapChainForComposition>(f2Vtable[24]);
                }
                f2Vtable[24]                         = reinterpret_cast<void*>(&HookedCreateSwapChainForComposition);
                VirtualProtect(&f2Vtable[24], sizeof(void*), oldProtect, &oldProtect);
                logMsg("[dgpu_dxgi] Intercepted IDXGIFactory2::CreateSwapChainForComposition vtable");
            }
        }
    }
}

} // namespace

// ─── Exported DXGI Proxy API (Exported via dxgi.def) ──────────────────────────

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    initSystemDxgi();
    logMsg("[dgpu_dxgi] Export CreateDXGIFactory called");
    if (!g_realCreateDXGIFactory) return E_FAIL;
    HRESULT hr = g_realCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        installFactoryHook(static_cast<IDXGIFactory*>(*ppFactory));
    }
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    initSystemDxgi();
    logMsg("[dgpu_dxgi] Export CreateDXGIFactory1 called");
    if (!g_realCreateDXGIFactory1) return E_FAIL;
    HRESULT hr = g_realCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        installFactoryHook(static_cast<IDXGIFactory*>(*ppFactory));
    }
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    initSystemDxgi();
    logMsg("[dgpu_dxgi] Export CreateDXGIFactory2 called");
    if (!g_realCreateDXGIFactory2) return E_FAIL;
    HRESULT hr = g_realCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        installFactoryHook(static_cast<IDXGIFactory*>(*ppFactory));
    }
    return hr;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    initSystemDxgi();
    if (!g_realDXGIDeclareAdapterRemovalSupport) return S_OK;
    return g_realDXGIDeclareAdapterRemovalSupport();
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    initSystemDxgi();
    if (!g_realDXGIGetDebugInterface1) return E_NOINTERFACE;
    return g_realDXGIGetDebugInterface1(Flags, riid, pDebug);
}

void WINAPI DgpuHookSwapChain(IDXGISwapChain* pSwapChain) {
    installSwapChainHook(pSwapChain, nullptr);
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
