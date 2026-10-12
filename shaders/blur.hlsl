// blur.hlsl
// Separable 5-tap Gaussian blur compute shader.
// Dispatch with 16x16 thread groups; one thread per pixel.
//
// Root signature:
//   Slot 0: UAV  u0  — output RGBA8 raw buffer
//   Slot 1: SRV  t0  — input  RGBA8 raw buffer
//   Slot 2: CBV  b0  — {Width, Height, Pad0, Pad1}

ByteAddressBuffer   InputBuf  : register(t0);
RWByteAddressBuffer OutputBuf : register(u0);

cbuffer Constants : register(b0) {
    uint Width;
    uint Height;
    uint Intensity;
    uint FrameIndex;
}

// Load a clamped-to-edge pixel and return it as packed uint32 RGBA8
uint LoadPixelClamped(int x, int y) {
    x = clamp(x, 0, (int)Width  - 1);
    y = clamp(y, 0, (int)Height - 1);
    return InputBuf.Load(((uint)y * Width + (uint)x) * 4u);
}

// Unpack uint32 RGBA8 → float4 [0,1]
float4 Unpack(uint p) {
    return float4(
        float((p      ) & 0xFFu) / 255.0f,
        float((p >>  8) & 0xFFu) / 255.0f,
        float((p >> 16) & 0xFFu) / 255.0f,
        float((p >> 24) & 0xFFu) / 255.0f
    );
}

// Pack float4 [0,1] → uint32 RGBA8
uint Pack(float4 c) {
    c = saturate(c);
    return (uint)(c.x * 255.0f)
         | ((uint)(c.y * 255.0f) <<  8u)
         | ((uint)(c.z * 255.0f) << 16u)
         | ((uint)(c.w * 255.0f) << 24u);
}

float GetLuma(float3 rgb) {
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= Width || DTid.y >= Height) return;

    int2 pos = int2(DTid.xy);
    float4 center = Unpack(LoadPixelClamped(pos.x, pos.y));

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
    float4 pL = Unpack(LoadPixelClamped(pos.x - 1, pos.y));
    float4 pR = Unpack(LoadPixelClamped(pos.x + 1, pos.y));
    float4 pT = Unpack(LoadPixelClamped(pos.x, pos.y - 1));
    float4 pB = Unpack(LoadPixelClamped(pos.x, pos.y + 1));
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
                float4 sampleCol = Unpack(LoadPixelClamped(pos.x + dx, pos.y + dy));
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
    float4 prevColor = Unpack(prevRaw);

    // Karis / TAA neighborhood AABB clamping: prevents ghosting on fast motion
    float3 boxMin = min(minColor, currentFrameResult);
    float3 boxMax = max(maxColor, currentFrameResult);
    float3 clampedHistory = clamp(prevColor.rgb, boxMin, boxMax);

    // Blend: if first frame or uninitialized, use 100% current; otherwise blend 84/16
    float temporalAlpha = (FrameIndex > 1u && prevRaw != 0u) ? 0.16f : 0.0f;
    float3 finalRgb = lerp(currentFrameResult, clampedHistory, temporalAlpha);

    OutputBuf.Store(pixelByteAddr, Pack(float4(finalRgb, center.a)));
}
