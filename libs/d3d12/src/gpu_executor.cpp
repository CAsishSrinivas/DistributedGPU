// gpu_executor/gpu_executor.cpp
// Dispatches Blur and EdgeDetection tasks on the local D3D12 device.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "d3d12/gpu_executor.hpp"
#include "d3d12/d3d12_context.hpp"
#include "telemetry/telemetry.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <mutex>
#include <unordered_map>

// ─── Embedded HLSL shaders ────────────────────────────────────────────────────
// These are compiled at runtime the first time GpuExecutor::init() is called.
// In a shipping build, replace with pre-compiled CSO bytes.

static const char* kBlurHlsl = R"hlsl(
// =============================================================================
// Advanced Cooperative GPU Post-Processing Pipeline:
// Pass 1: Multi-Iteration Bilateral Filter & Directional Edge Enhancement
// Pass 2: Contrast-Adaptive Sharpening (AMD FidelityFX CAS)
// Pass 3: Heavy Compute Shader Bloom Synthesis & Temporal History Accumulation Pass
// =============================================================================

ByteAddressBuffer   InputBuf  : register(t0);
RWByteAddressBuffer OutputBuf : register(u0);
cbuffer Constants : register(b0) { uint Width; uint Height; uint Intensity; uint FrameIndex; }

uint LoadPixel(int x, int y) {
    x = clamp(x, 0, (int)Width  - 1);
    y = clamp(y, 0, (int)Height - 1);
    return InputBuf.Load(((uint)y * Width + (uint)x) * 4u);
}

float4 UnpackRGBA(uint p) {
    return float4(
        float((p      ) & 0xFFu) / 255.0f,
        float((p >>  8) & 0xFFu) / 255.0f,
        float((p >> 16) & 0xFFu) / 255.0f,
        float((p >> 24) & 0xFFu) / 255.0f);
}

uint PackRGBA(float4 c) {
    c = saturate(c);
    return ((uint)(c.x * 255.0f))        |
           ((uint)(c.y * 255.0f) <<  8u)  |
           ((uint)(c.z * 255.0f) << 16u)  |
           ((uint)(c.w * 255.0f) << 24u);
}

float GetLuma(float3 rgb) {
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;

    int2 pos = int2(id.xy);
    float4 center = UnpackRGBA(LoadPixel(pos.x, pos.y));

    // ─────────────────────────────────────────────────────────────────────────
    // PASS 1: Multi-Iteration Bilateral Filtering & Edge Enhancement
    // ─────────────────────────────────────────────────────────────────────────
    uint radius = Intensity & 0xFFu;
    if (radius == 0u) radius = 9u;
    uint numIters = (Intensity >> 8u) & 0xFFu;
    if (numIters == 0u) numIters = 16u;

    int rad = int(radius);
    float spatialSigma = max(2.0f, float(rad) * 0.5f);
    float spatialFactor = -0.5f / (spatialSigma * spatialSigma);

    // Initial gradient estimation & structure tensor
    float4 pL = UnpackRGBA(LoadPixel(pos.x - 1, pos.y));
    float4 pR = UnpackRGBA(LoadPixel(pos.x + 1, pos.y));
    float4 pT = UnpackRGBA(LoadPixel(pos.x, pos.y - 1));
    float4 pB = UnpackRGBA(LoadPixel(pos.x, pos.y + 1));
    float3 gx = pR.rgb - pL.rgb;
    float3 gy = pB.rgb - pT.rgb;
    float gradMag = sqrt(dot(gx, gx) + dot(gy, gy));
    float edgeFactor = saturate(gradMag * 3.8f);

    float3 currentEstimate = center.rgb;
    float3 bloomAccTotal = float3(0, 0, 0);
    float bloomWeightTotal = 0.0f;

    [loop]
    for (uint iter = 0; iter < numIters; ++iter) {
        // Adapts color sigma per iteration for progressive refinement
        float progress = float(iter) / float(max(numIters - 1u, 1u));
        float colorSigma = lerp(0.24f, 0.10f, progress);
        float colorFactor = -0.5f / (colorSigma * colorSigma);

        float4 bilateralAcc = float4(0, 0, 0, 0);
        float bilateralWeight = 0.0f;

        [loop]
        for (int dy = -rad; dy <= rad; ++dy) {
            [loop]
            for (int dx = -rad; dx <= rad; ++dx) {
                float4 sampleCol = UnpackRGBA(LoadPixel(pos.x + dx, pos.y + dy));
                float dist2 = float(dx * dx + dy * dy);

                // Bilateral photometric weight using progressive estimate
                float3 diff = sampleCol.rgb - currentEstimate;
                float colorDist2 = dot(diff, diff);
                float w = exp(dist2 * spatialFactor + colorDist2 * colorFactor);

                bilateralAcc += sampleCol * w;
                bilateralWeight += w;

                // Heavy Bloom synthesis on each iteration
                float luma = GetLuma(sampleCol.rgb);
                if (luma > 0.58f) {
                    float bloomDistW = exp(-dist2 / (2.5f * spatialSigma * spatialSigma));
                    float softKnee = saturate((luma - 0.58f) / 0.42f);
                    float3 bloomEnergy = sampleCol.rgb * (softKnee * softKnee);
                    bloomAccTotal += bloomEnergy * bloomDistW;
                    bloomWeightTotal += bloomDistW;
                }
            }
        }

        float3 bilateralResult = (bilateralWeight > 0.0001f) ? (bilateralAcc.rgb / bilateralWeight) : currentEstimate;

        // Progressive edge enhancement
        float3 edgeDiff = center.rgb - bilateralResult;
        currentEstimate = saturate(bilateralResult + edgeDiff * (0.50f + 0.35f * edgeFactor));
    }

    float3 enhancedBase = currentEstimate;

    // ─────────────────────────────────────────────────────────────────────────
    // PASS 2: Contrast-Adaptive Sharpening (AMD FidelityFX CAS)
    // ─────────────────────────────────────────────────────────────────────────
    // Sample cross neighbors (a=up, b=down, c=left, d=right, e=center)
    float3 a = pT.rgb;
    float3 b = pB.rgb;
    float3 c = pL.rgb;
    float3 d = pR.rgb;
    float3 e = enhancedBase;

    // Local min/max bounds to strictly eliminate overshoot and halos
    float3 minColor = min(e, min(min(a, b), min(c, d)));
    float3 maxColor = max(e, max(max(a, b), max(c, d)));

    // Non-linear contrast amplification ratio
    float3 ampRatio = saturate(min(minColor, 2.0f - maxColor) / max(maxColor, 0.0001f));
    float3 wCAS = -sqrt(ampRatio) * 0.20f; // Peak adaptive sharpness coefficient

    // CAS reconstruction filter
    float3 sharpened = saturate((a * wCAS + b * wCAS + c * wCAS + d * wCAS + e) / (1.0f + 4.0f * wCAS));

    // Combine sharpened image with cumulative bloom glow
    float3 bloomColor = (bloomWeightTotal > 0.0001f) ? (bloomAccTotal / bloomWeightTotal) * 1.45f : float3(0, 0, 0);
    float3 currentFrameResult = saturate(sharpened + bloomColor);

    // ─────────────────────────────────────────────────────────────────────────
    // PASS 3 (Temporal): Temporal History Pass with AABB Neighborhood Clamping
    // ─────────────────────────────────────────────────────────────────────────
    uint pixelByteAddr = ((uint)pos.y * Width + (uint)pos.x) * 4u;
    uint prevRaw = OutputBuf.Load(pixelByteAddr);
    float4 prevColor = UnpackRGBA(prevRaw);

    // Karis / TAA neighborhood AABB clamping: prevents ghosting on fast motion
    float3 boxMin = min(minColor, currentFrameResult);
    float3 boxMax = max(maxColor, currentFrameResult);
    float3 clampedHistory = clamp(prevColor.rgb, boxMin, boxMax);

    // Blend: if first frame or uninitialized, use 100% current; otherwise blend 84/16
    float temporalAlpha = (FrameIndex > 1u && prevRaw != 0u) ? 0.16f : 0.0f;
    float3 finalRgb = lerp(currentFrameResult, clampedHistory, temporalAlpha);

    OutputBuf.Store(pixelByteAddr, PackRGBA(float4(finalRgb, center.a)));
}
)hlsl";

static const char* kEdgeHlsl = R"hlsl(
// edge_detect.hlsl — Sobel edge detection
ByteAddressBuffer   InputBuf  : register(t0);
RWByteAddressBuffer OutputBuf : register(u0);
cbuffer Constants : register(b0) { uint Width; uint Height; uint Pad0; uint Pad1; }

uint LoadPixel(uint x, uint y) {
    x = clamp(x, 0u, Width  - 1u);
    y = clamp(y, 0u, Height - 1u);
    return InputBuf.Load((y * Width + x) * 4);
}

float Luma(uint p) {
    float r = float((p      ) & 0xFF) / 255.0;
    float g = float((p >>  8) & 0xFF) / 255.0;
    float b = float((p >> 16) & 0xFF) / 255.0;
    return 0.299 * r + 0.587 * g + 0.114 * b;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Width || id.y >= Height) return;
    float gx = -Luma(LoadPixel(id.x-1,id.y-1)) - 2*Luma(LoadPixel(id.x-1,id.y)) - Luma(LoadPixel(id.x-1,id.y+1))
               +Luma(LoadPixel(id.x+1,id.y-1)) + 2*Luma(LoadPixel(id.x+1,id.y)) + Luma(LoadPixel(id.x+1,id.y+1));
    float gy = -Luma(LoadPixel(id.x-1,id.y-1)) - 2*Luma(LoadPixel(id.x,id.y-1)) - Luma(LoadPixel(id.x+1,id.y-1))
               +Luma(LoadPixel(id.x-1,id.y+1)) + 2*Luma(LoadPixel(id.x,id.y+1)) + Luma(LoadPixel(id.x+1,id.y+1));
    float mag = saturate(sqrt(gx*gx + gy*gy));
    uint  edgeVal = (uint)(mag * 255.0);
    edgeVal = edgeVal | (edgeVal << 8) | (edgeVal << 16) | (0xFF << 24);
    OutputBuf.Store((id.y * Width + id.x) * 4, edgeVal);
}
)hlsl";

namespace dgpu::gpu_executor {

using telemetry::ScopedTimer;

// ─── GpuExecutor::Impl ───────────────────────────────────────────────────────

struct BufferSet {
    d3d12::GpuBuffer upload;
    d3d12::GpuBuffer gpuInput;
    d3d12::GpuBuffer gpuOutput;
    d3d12::GpuBuffer readback;
    uint32_t         width{};
    uint32_t         height{};
    bool             valid{false};
    D3D12_RESOURCE_STATES gpuInputState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES gpuOutputState{D3D12_RESOURCE_STATE_COMMON};
};

struct GpuExecutor::Impl {
    d3d12::D3D12Context                                       ctx;
    std::unordered_map<uint32_t, d3d12::ComputePipeline>     pipelines; // keyed by TaskType
    d3d12::GpuTimer                                           timer;
    BufferSet                                                 bufCache;
    std::mutex                                                mu; // single-executor mutex

    bool ensureBuffers(uint32_t w, uint32_t h) {
        if (bufCache.valid && bufCache.width == w && bufCache.height == h)
            return true;

        const uint64_t sz = static_cast<uint64_t>(w) * h * 4;
        bufCache.upload  = {};
        bufCache.gpuInput= {};
        bufCache.gpuOutput={};
        bufCache.readback= {};

        if (!bufCache.upload.create(ctx.device(), sz, d3d12::BufferUsage::Upload))   return false;
        if (!bufCache.gpuInput.create(ctx.device(), sz, d3d12::BufferUsage::Default)) return false;
        if (!bufCache.gpuOutput.create(ctx.device(), sz, d3d12::BufferUsage::Default))return false;
        if (!bufCache.readback.create(ctx.device(), sz, d3d12::BufferUsage::Readback))return false;

        bufCache.width  = w;
        bufCache.height = h;
        bufCache.valid  = true;
        bufCache.gpuInputState  = D3D12_RESOURCE_STATE_COMMON;
        bufCache.gpuOutputState = D3D12_RESOURCE_STATE_COMMON;
        return true;
    }
};

// ─── GpuExecutor ─────────────────────────────────────────────────────────────

GpuExecutor::GpuExecutor()  : m_impl(std::make_unique<Impl>()) {}
GpuExecutor::~GpuExecutor() = default;

bool GpuExecutor::init(const std::string& preferredAdapter) {
    if (!m_impl->ctx.init(preferredAdapter)) return false;
    if (!m_impl->timer.init(m_impl->ctx.device(), m_impl->ctx.cmdQueue())) return false;

    // Pre-compile both shaders
    d3d12::ComputePipeline blurPso, edgePso;
    if (!blurPso.compileFromSource(m_impl->ctx.device(), kBlurHlsl)) {
        std::cerr << "[GpuExecutor] Failed to compile blur shader\n";
        return false;
    }
    if (!edgePso.compileFromSource(m_impl->ctx.device(), kEdgeHlsl)) {
        std::cerr << "[GpuExecutor] Failed to compile edge shader\n";
        return false;
    }

    m_impl->pipelines[static_cast<uint32_t>(protocol::TaskType::Blur)]
        = std::move(blurPso);
    m_impl->pipelines[static_cast<uint32_t>(protocol::TaskType::EdgeDetection)]
        = std::move(edgePso);

    return true;
}

static void transitionResource(ID3D12GraphicsCommandList* cl,
                               ID3D12Resource* res,
                               D3D12_RESOURCE_STATES& curState,
                               D3D12_RESOURCE_STATES targetState) {
    if (curState == targetState) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = curState;
    b.Transition.StateAfter  = targetState;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    curState = targetState;
}

protocol::TaskResult GpuExecutor::execute(const protocol::TaskRequest& req) {
    std::lock_guard<std::mutex> lk(m_impl->mu);

    protocol::TaskResult result;
    result.taskId = req.taskId;

    if (!req.validate()) {
        result.status = protocol::TaskStatus::InvalidRequest;
        return result;
    }

    auto it = m_impl->pipelines.find(static_cast<uint32_t>(req.type));
    if (it == m_impl->pipelines.end()) {
        result.status = protocol::TaskStatus::UnsupportedTask;
        return result;
    }

    auto totalStart = std::chrono::steady_clock::now();

    // ── 1. Upload Staging (CPU -> Upload Buffer) ──────────────────────────────
    auto uploadStart = std::chrono::steady_clock::now();
    if (!m_impl->ensureBuffers(req.width, req.height)) {
        result.status = protocol::TaskStatus::GpuError;
        return result;
    }
    if (!m_impl->bufCache.upload.upload(req.input)) {
        result.status = protocol::TaskStatus::GpuError;
        return result;
    }
    auto uploadEnd = std::chrono::steady_clock::now();
    result.uploadUs = std::chrono::duration_cast<std::chrono::microseconds>(
                          uploadEnd - uploadStart).count();

    // ── 2. Unified GPU Execution (Upload Copy -> Compute Dispatch -> Readback Copy) ──
    if (!m_impl->ctx.resetCommandList()) {
        result.status = protocol::TaskStatus::GpuError;
        return result;
    }
    auto* cl = m_impl->ctx.cmdList();

    // 2a. Copy from upload heap to default GPU input buffer
    transitionResource(cl, m_impl->bufCache.gpuInput.resource(),
                       m_impl->bufCache.gpuInputState,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(m_impl->bufCache.gpuInput.resource(),
                     m_impl->bufCache.upload.resource());

    // 2b. Transition resources for compute dispatch
    transitionResource(cl, m_impl->bufCache.gpuInput.resource(),
                       m_impl->bufCache.gpuInputState,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionResource(cl, m_impl->bufCache.gpuOutput.resource(),
                       m_impl->bufCache.gpuOutputState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // 2c. Bind compute pipeline and root descriptors
    auto& pso = it->second;
    pso.bind(cl);
    pso.setRootUAV(cl, m_impl->bufCache.gpuOutput.resource()->GetGPUVirtualAddress(), 0);
    pso.setRootSRV(cl, m_impl->bufCache.gpuInput.resource()->GetGPUVirtualAddress(), 1);

    static uint32_t s_radius = 0;
    static uint32_t s_iterations = 0;
    if (s_radius == 0) {
        char envVal[32] = {0};
        if (GetEnvironmentVariableA("DGPU_INTENSITY", envVal, sizeof(envVal)) > 0) {
            s_radius = static_cast<uint32_t>(std::atoi(envVal));
        }
        if (s_radius == 0) s_radius = 9; // Default radius 9 (19x19 = 361 samples)
    }
    if (s_iterations == 0) {
        char envVal[32] = {0};
        if (GetEnvironmentVariableA("DGPU_ITERATIONS", envVal, sizeof(envVal)) > 0) {
            s_iterations = static_cast<uint32_t>(std::atoi(envVal));
        }
        if (s_iterations == 0) s_iterations = 14; // Default 14 iterations
    }
    uint32_t packedConfig = (s_radius & 0xFFu) | ((s_iterations & 0xFFu) << 8u);

    static uint32_t s_frameCounter = 0;
    ++s_frameCounter;
    uint32_t constants[4] = {req.width, req.height, packedConfig, s_frameCounter};
    pso.setRootConstants(cl, constants, 4, 2);

    // 2d. Dispatch with GPU timestamp query
    m_impl->timer.begin(cl);
    const uint32_t gx = (req.width  + 15) / 16;
    const uint32_t gy = (req.height + 15) / 16;
    cl->Dispatch(gx, gy, 1);
    m_impl->timer.end(cl);

    // 2e. Transition output buffer to COPY_SOURCE and copy to readback buffer
    transitionResource(cl, m_impl->bufCache.gpuOutput.resource(),
                       m_impl->bufCache.gpuOutputState,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(m_impl->bufCache.readback.resource(),
                     m_impl->bufCache.gpuOutput.resource());

    // 2f. Restore GPU resources to COMMON
    transitionResource(cl, m_impl->bufCache.gpuInput.resource(),
                       m_impl->bufCache.gpuInputState,
                       D3D12_RESOURCE_STATE_COMMON);
    transitionResource(cl, m_impl->bufCache.gpuOutput.resource(),
                       m_impl->bufCache.gpuOutputState,
                       D3D12_RESOURCE_STATE_COMMON);

    // Single unified GPU execution & fence wait
    m_impl->ctx.executeAndWait();
    result.gpuUs = m_impl->timer.elapsedMicroseconds();

    // ── 3. Readback Staging (Readback Buffer -> CPU) ───────────────────────────
    auto readbackStart = std::chrono::steady_clock::now();
    if (!m_impl->bufCache.readback.readback(result.output)) {
        result.status = protocol::TaskStatus::GpuError;
        return result;
    }
    auto readbackEnd = std::chrono::steady_clock::now();
    result.readbackUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            readbackEnd - readbackStart).count();

    auto totalEnd = std::chrono::steady_clock::now();
    result.totalUs = std::chrono::duration_cast<std::chrono::microseconds>(
                         totalEnd - totalStart).count();
    result.status  = protocol::TaskStatus::Ok;
    return result;
}

const d3d12::AdapterInfo& GpuExecutor::adapterInfo() const noexcept {
    return m_impl->ctx.adapterInfo();
}

} // namespace dgpu::gpu_executor
