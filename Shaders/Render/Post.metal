// Shaders/Render/Post.metal
// Scene-level post on the final render target, enabled by a top-level "post"
// block in the scene JSON (no block = these never run). Bloom only, for now:
// bright-pass box downsample to 1/4 res, separable gaussian there, add back.
// All the blur work happens on 1/16 of the pixels, so it stays cheap on very
// wide targets. x wraps and y clamps, like Relief.metal. Matches life::BloomParams.

struct BloomParams {
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;   // quarter-res size
    uint dstHeight;
    float threshold; // linear luminance where bloom starts
    float knee;      // soft-knee width above the threshold
    float sigma;     // gaussian sigma in QUARTER-res px
    float intensity;
    uint horizontal; // blur direction
};

kernel void bloomDown(texture2d<float, access::read> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      constant BloomParams& p [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.dstWidth || gid.y >= p.dstHeight) return;
    float3 sum = float3(0.0f);
    for (uint dy = 0u; dy < 4u; ++dy) {
        for (uint dx = 0u; dx < 4u; ++dx) {
            uint2 q = uint2(min(gid.x * 4u + dx, p.srcWidth - 1u), min(gid.y * 4u + dy, p.srcHeight - 1u));
            float3 c = src.read(q).rgb;
            float l = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
            // Soft knee: nothing below threshold, full above threshold + knee.
            float w = smoothstep(p.threshold, p.threshold + max(p.knee, 1e-4f), l);
            sum += c * w;
        }
    }
    dst.write(float4(sum / 16.0f, 1.0f), gid);
}

kernel void bloomBlur(texture2d<float, access::read> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      constant BloomParams& p [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.dstWidth || gid.y >= p.dstHeight) return;
    int radius = int(ceil(2.5f * p.sigma));
    int2 dir = p.horizontal != 0u ? int2(1, 0) : int2(0, 1);
    float3 sum = float3(0.0f);
    float wsum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        float w = exp(-0.5f * float(i * i) / (p.sigma * p.sigma));
        sum += src.read(edgeCoord(int2(gid) + dir * i, p.dstWidth, p.dstHeight, 1u)).rgb * w;
        wsum += w;
    }
    dst.write(float4(sum / wsum, 1.0f), gid);
}

kernel void bloomAdd(texture2d<float, access::sample> bloom [[texture(0)]],
                     texture2d<float, access::read_write> target [[texture(1)]],
                     constant BloomParams& p [[buffer(0)]],
                     uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.srcWidth || gid.y >= p.srcHeight) return;
    constexpr sampler s(filter::linear, s_address::repeat, t_address::clamp_to_edge, coord::normalized);
    float2 uv = (float2(gid) + 0.5f) / float2(p.srcWidth, p.srcHeight);
    float4 c = target.read(gid);
    target.write(float4(c.rgb + bloom.sample(s, uv).rgb * p.intensity, c.a), gid);
}
