// Shaders/Render/Present.metal
// Linear RGBA16F render target -> sRGB-encoded BGRA8Unorm presentation
// texture, shared by WindowPreviewSink and SyphonSink (design doc §3.8 output
// abstraction). The encode curve mirrors LifeCore/IO/ImageWriter.mm's
// linearToSRGB8 by hand — keep the two in sync.

struct PresentParams {
    uint width;   // dst width (dispatch grid == dst size)
    uint height;  // dst height
    float exposure;
};

inline float presentLinearToSRGB(float c) {
    c = saturate(c);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

// dst may differ in size from src (e.g. a differently-sized intermediate);
// gid iterates the dst grid and is nearest-scaled back into src space.
kernel void presentToBGRA(texture2d<float, access::read> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant PresentParams& p [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    uint srcW = src.get_width();
    uint srcH = src.get_height();
    uint2 srcCoord = uint2(min(srcW - 1, (gid.x * srcW) / max(p.width, 1u)),
                          min(srcH - 1, (gid.y * srcH) / max(p.height, 1u)));

    float4 c = src.read(srcCoord) * p.exposure;
    // texture write on a BGRA8Unorm destination still takes its argument in
    // RGBA logical-channel order; Metal handles the storage swizzle.
    float4 out = float4(presentLinearToSRGB(c.r), presentLinearToSRGB(c.g),
                        presentLinearToSRGB(c.b), 1.0f);
    dst.write(out, gid);
}
