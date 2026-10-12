// edge_detect.hlsl
// Sobel edge-detection compute shader.
// Dispatch with 16x16 thread groups; one thread per pixel.
//
// Root signature: same as blur.hlsl

ByteAddressBuffer   InputBuf  : register(t0);
RWByteAddressBuffer OutputBuf : register(u0);

cbuffer Constants : register(b0) {
    uint Width;
    uint Height;
    uint Pad0;
    uint Pad1;
}

uint LoadPixelClamped(int x, int y) {
    x = clamp(x, 0, (int)Width  - 1);
    y = clamp(y, 0, (int)Height - 1);
    return InputBuf.Load(((uint)y * Width + (uint)x) * 4u);
}

// Convert RGBA8 packed uint to luminance [0,1]
float Luma(uint p) {
    float r = float((p      ) & 0xFFu) / 255.0f;
    float g = float((p >>  8) & 0xFFu) / 255.0f;
    float b = float((p >> 16) & 0xFFu) / 255.0f;
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= Width || DTid.y >= Height) return;

    int x = (int)DTid.x;
    int y = (int)DTid.y;

    // Sobel kernels
    float gx =
        -1.0f * Luma(LoadPixelClamped(x-1, y-1)) +
        -2.0f * Luma(LoadPixelClamped(x-1, y  )) +
        -1.0f * Luma(LoadPixelClamped(x-1, y+1)) +
         1.0f * Luma(LoadPixelClamped(x+1, y-1)) +
         2.0f * Luma(LoadPixelClamped(x+1, y  )) +
         1.0f * Luma(LoadPixelClamped(x+1, y+1));

    float gy =
        -1.0f * Luma(LoadPixelClamped(x-1, y-1)) +
        -2.0f * Luma(LoadPixelClamped(x,   y-1)) +
        -1.0f * Luma(LoadPixelClamped(x+1, y-1)) +
         1.0f * Luma(LoadPixelClamped(x-1, y+1)) +
         2.0f * Luma(LoadPixelClamped(x,   y+1)) +
         1.0f * Luma(LoadPixelClamped(x+1, y+1));

    float mag  = saturate(sqrt(gx * gx + gy * gy));
    uint  grey = (uint)(mag * 255.0f);

    // Output greyscale edge magnitude; alpha = 255
    uint packed = grey | (grey << 8u) | (grey << 16u) | (0xFFu << 24u);
    OutputBuf.Store((DTid.y * Width + DTid.x) * 4u, packed);
}
