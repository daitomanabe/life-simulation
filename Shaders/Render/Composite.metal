// Shaders/Render/Composite.metal
// Layer compositing into the final RGBA16F render target (design doc §13.2).

struct CompositeParams {
    uint mode; // 0 add, 1 screen, 2 multiply, 3 max, 4 alpha
    float opacity;
};

struct ClearParams {
    float r, g, b, a;
};

kernel void clearTexture(texture2d<float, access::write> dst [[texture(0)]],
                         constant ClearParams& p [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    dst.write(float4(p.r, p.g, p.b, p.a), gid);
}

kernel void compositeLayer(texture2d<float, access::read> src [[texture(0)]],
                           texture2d<float, access::read_write> dst [[texture(1)]],
                           constant CompositeParams& p [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;

    float4 s = src.read(gid);
    float4 d = dst.read(gid);
    float3 result;

    switch (p.mode) {
        case 1: // screen
            result = 1.0f - (1.0f - d.rgb) * (1.0f - s.rgb * p.opacity);
            break;
        case 2: // multiply
            result = d.rgb * mix(float3(1.0f), s.rgb, p.opacity);
            break;
        case 3: // max
            result = max(d.rgb, s.rgb * p.opacity);
            break;
        case 4: { // alpha (straight, source alpha × opacity)
            float a = clamp(s.a * p.opacity, 0.0f, 1.0f);
            result = mix(d.rgb, s.rgb, a);
            break;
        }
        default: // add
            result = d.rgb + s.rgb * p.opacity;
            break;
    }

    dst.write(float4(result, 1.0f), gid);
}
