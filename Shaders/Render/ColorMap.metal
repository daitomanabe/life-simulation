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
