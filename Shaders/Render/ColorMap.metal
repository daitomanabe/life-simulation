// Shaders/Render/ColorMap.metal
// Field scalar → RGBA16F color layer via cosine palette:
//   color(t) = a + b * cos(2π (c*t + d))     (Inigo Quilez)
// Matches life::ColorMapParams (scalar-packed).

struct ColorMapParams {
    float aR, aG, aB;
    float bR, bG, bB;
    float cR, cG, cB;
    float dR, dG, dB;
    float inputScale;
    float inputBias;
    float exposure;
    uint channel;
};

kernel void colorMapField(texture2d<float, access::read> field [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant ColorMapParams& p [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;

    float4 v = field.read(gid);
    float t = v[min(p.channel, 3u)] * p.inputScale + p.inputBias;
    t = clamp(t, 0.0f, 1.0f);

    float3 a = float3(p.aR, p.aG, p.aB);
    float3 b = float3(p.bR, p.bG, p.bB);
    float3 c = float3(p.cR, p.cG, p.cB);
    float3 d = float3(p.dR, p.dG, p.dB);
    float3 rgb = a + b * cos(6.28318530718f * (c * t + d));
    rgb = max(rgb, 0.0f) * p.exposure;

    // Keep t in alpha — useful for downstream alpha compositing.
    dst.write(float4(rgb, t), gid);
}

// ---- phase9 (docs/specs/phase9_lenia_fft.md §1): size-independent variant
// of colorMapField above. field is bound access::sample and read with a
// normalized-coordinate bilinear lookup instead of a same-size access::read,
// so `field` and `dst` may be different sizes (Lenia's sim resolution vs the
// scene's output resolution). colorMapField above is unchanged and still
// used by every other module, and by Lenia itself whenever sim resolution
// equals scene resolution (the default) — see ColorMapPass::encodeScaled.
struct ColorMapScaledParams {
    uint dstWidth;
    uint dstHeight;
};

kernel void colorMapFieldScaled(texture2d<float, access::sample> field [[texture(0)]],
                                texture2d<float, access::write> dst [[texture(1)]],
                                constant ColorMapParams& p [[buffer(0)]],
                                constant ColorMapScaledParams& sp [[buffer(1)]],
                                uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= sp.dstWidth || gid.y >= sp.dstHeight) return;

    // Sampling at the exact texel center of a same-size source is exact (no
    // blend across neighbors — see Sampling.metal / phase9 spec §1), so this
    // formula is correct whether or not field and dst share a size.
    float2 uv = (float2(gid) + 0.5f) / float2(sp.dstWidth, sp.dstHeight);
    constexpr sampler s(filter::linear, address::repeat, coord::normalized);
    float4 v = field.sample(s, uv);

    float t = v[min(p.channel, 3u)] * p.inputScale + p.inputBias;
    t = clamp(t, 0.0f, 1.0f);

    float3 a = float3(p.aR, p.aG, p.aB);
    float3 b = float3(p.bR, p.bG, p.bB);
    float3 c = float3(p.cR, p.cG, p.cB);
    float3 d = float3(p.dR, p.dG, p.dB);
    float3 rgb = a + b * cos(6.28318530718f * (c * t + d));
    rgb = max(rgb, 0.0f) * p.exposure;

    dst.write(float4(rgb, t), gid);
}
