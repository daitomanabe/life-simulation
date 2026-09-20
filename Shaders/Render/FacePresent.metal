// Shaders/Render/FacePresent.metal
// Per-face upscale/present for Room B's continuous 3-wall strip (west/north/
// east — see FaceSyphonSink): face k of the RGBA16F render target, source x
// range [k*srcW/N, (k+1)*srcW/N) x full height, is bilinear-upscaled (e.g.
// 5460x692 -> 6816x864, ~1.248x) straight into a BGRA8Unorm per-face output.
// One dispatch per face; FaceSyphonSink publishes each to its own Syphon
// server.
//
// Bilinear, not the nearest-scaled read presentToBGRA uses: at this upscale
// factor nearest would alias the 1-3px contour lines and single-pixel
// particles that dominate this picture. x wraps (matches the sim's own
// toroidal x and keeps the west/east seam continuous); y clamps, like every
// other x-wrap/y-clamp field read in this suite (Fluid, Relief, Post bloom).
//
// Exposure + the linear->sRGB encode must match presentToBGRA exactly, so
// the curve is forward-declared here and shared from Present.metal rather
// than copied. presentToBGRA itself is left untouched — this is a new
// kernel only.
//
// Film grain lives HERE, not in reliefShadeField, precisely because this is
// the last stop before the upscale, not before it. Grain added at sim
// resolution (16380x692) and then bilinear-upscaled 1.2483x comes out
// correlated across neighbours (measured: +0.42/+0.43 adjacent-pixel
// correlation x/y, std dev drops from 1.52 to 1.01) — the resample blends
// each output pixel from a handful of sim-resolution noise samples, so it
// stops being independent noise and starts being a soft blotchy pattern.
// Generating the jitter from gid — the OUTPUT pixel coordinate, post-sample
// — instead keeps every output pixel's noise independent of its neighbours,
// like the reference white-noise-at-output-resolution row in that same
// measurement (std 0.58, correlation ~0). grain 0 (default) skips the
// rand01 call entirely, so it costs nothing and is bit-identical to no grain
// at all.

inline float presentLinearToSRGB(float c);

struct FacePresentParams {
    uint dstWidth;   // this face's output width (e.g. 6816)
    uint dstHeight;  // this face's output height (e.g. 864)
    float uOffset;   // this face's left edge, normalized full-strip u in [0,1)
    float uScale;    // this face's width, normalized full-strip u (1/faceCount)
    float exposure;
    float grain;     // multiplicative jitter 1 + grain*(rand-0.5); same meaning as
                     // ReliefParams.grain (Relief.metal), just applied post-upscale
                     // instead of pre-upscale. 0 = off (default, true no-op).
    uint frame;      // grain reseed, unique per face + per publish() call (see
                     // FaceSyphonSink::publish) — NOT a wall clock.
};

kernel void facePresentToBGRA(texture2d<float, access::sample> src [[texture(0)]],
                              texture2d<float, access::write> dst [[texture(1)]],
                              constant FacePresentParams& p [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.dstWidth || gid.y >= p.dstHeight) return;

    constexpr sampler s(filter::linear, s_address::repeat, t_address::clamp_to_edge,
                        coord::normalized);
    float2 uv = float2(p.uOffset + (float(gid.x) + 0.5f) / float(p.dstWidth) * p.uScale,
                       (float(gid.y) + 0.5f) / float(p.dstHeight));
    float4 c = src.sample(s, uv) * p.exposure;
    // Same rand01 helper Relief.metal's reliefShadeField uses for
    // relief.grain, same formula — keyed on gid (this output pixel) and
    // p.frame instead of the sim-resolution pixel, so it lands after the
    // upscale instead of before it. One rand01 call shared across r/g/b
    // keeps the grain achromatic (luminance jitter, not color noise), which
    // is what relief.grain produced too (it scaled the pre-tint scalar `v`).
    if (p.grain > 0.0f) {
        float jitter = 1.0f + p.grain * (rand01(gid, 0x67726Fu, p.frame) - 0.5f);
        c.rgb *= jitter;
    }
    // texture write on a BGRA8Unorm destination still takes its argument in
    // RGBA logical-channel order; Metal handles the storage swizzle (see
    // presentToBGRA).
    float4 out = float4(presentLinearToSRGB(c.r), presentLinearToSRGB(c.g),
                        presentLinearToSRGB(c.b), 1.0f);
    dst.write(out, gid);
}
