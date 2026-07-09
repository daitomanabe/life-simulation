// Shaders/Field/Lenia.metal
// Single-channel Lenia: direct radial-kernel convolution + gaussian growth,
// fused into one pass. Kernel weights come pre-normalized (Σ=1) in a small
// R32F texture. Wrap boundary (toroidal world).
//
//   u   = Σ K(dx,dy) · A(x+dx, y+dy)          (potential)
//   G   = 2·exp(-(u-mu)²/(2σ²)) - 1           (growth, [-1,1])
//   A'  = clamp(A + dt·G, 0, 1)

struct LeniaParams {
    float dt;
    float growthMu;
    float growthSigma;
    float noiseAmount;
    float muJitter;
    float initCoverage;
    float initScale;
    float injectAmount; // strength of seeded blob injection (kick → birth)
    uint injectCount;   // blobs per injection cycle
    uint radius;
    uint kernelSize;
    uint seed;
    uint width;
    uint height;
    uint frameIndex;
    uint growthMode; // 0 standard (2·bell−1), 1 asymptotic (bell − A)
};

// Smoothed value-noise soup — the classic Lenia random init (uniform noise
// blurred to kernel scale). Blobby saturated inits die from the inside
// (u ≈ 1 → G = -1); mid-level multi-octave noise keeps u spread around mu.
static float leniaValueNoise(float2 p, uint tag, uint seed) {
    float2 i = floor(p) + 512.0f; // keep lattice coords positive
    float2 f = fract(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = rand01(uint2(i), tag, seed);
    float b = rand01(uint2(i + float2(1, 0)), tag, seed);
    float c = rand01(uint2(i + float2(0, 1)), tag, seed);
    float d = rand01(uint2(i + float2(1, 1)), tag, seed);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

kernel void leniaInit(texture2d<float, access::write> state [[texture(0)]],
                      constant LeniaParams& p [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    // initScale = base noise wavelength in pixels (≈ kernel diameter works
    // well); three octaves, remapped by initCoverage.
    float2 px = float2(gid) / max(p.initScale, 2.0f);
    float v = 0.55f * leniaValueNoise(px, 11u, p.seed) +
              0.30f * leniaValueNoise(px * 2.13f, 23u, p.seed) +
              0.15f * leniaValueNoise(px * 4.31f, 37u, p.seed);

    // Threshold-free contrast: push values toward 0 outside "islands" while
    // keeping interiors mid-level (never saturated at 1).
    v = clamp((v - (1.0f - p.initCoverage)) / max(p.initCoverage, 1e-3f), 0.0f, 1.0f);
    v *= 0.9f;

    state.write(float4(v, 0.0f, 0.0f, 0.0f), gid);
}

kernel void leniaStep(texture2d<float, access::read> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      texture2d<float, access::read> kern [[texture(2)]],
                      constant LeniaParams& p [[buffer(0)]],
                      constant AudioUniforms& audio [[buffer(1)]],
                      uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    const int R = int(p.radius);
    int2 base = int2(gid);

    // Direct convolution over the (2R+1)² window, toroidal.
    float u = 0.0f;
    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            float w = kern.read(uint2(dx + R, dy + R)).x;
            if (w == 0.0f) continue;
            uint2 q = wrapCoord(base + int2(dx, dy), p.width, p.height);
            u += w * src.read(q).x;
        }
    }

    float A = src.read(gid).x;

    // Rule modulation: muJitter shifts the growth center per-pixel (snare →
    // topology break), noiseAmount stirs the state (hihat → shimmer).
    float mu = p.growthMu;
    if (p.muJitter > 0.0f) {
        float j = rand01(gid, 401u + audio.frameIndex % 613u, p.seed) - 0.5f;
        mu += j * p.muJitter * 0.05f;
    }

    float t = (u - mu) / p.growthSigma;
    float bell = exp(-0.5f * t * t);
    // Standard Lenia growth oscillates and can collapse from random soups;
    // asymptotic Lenia (A relaxes toward the bell target) self-sustains.
    float A2 = (p.growthMode == 1u) ? A + p.dt * (bell - A)
                                    : A + p.dt * (2.0f * bell - 1.0f);
    if (p.noiseAmount > 0.0f) {
        float n = rand01(gid, 503u, p.seed) - 0.5f;
        A2 += n * p.noiseAmount * 0.03f;
    }

    // Seeded blob injection — audio-driven birth (design doc §1.2: kick
    // changes birth rate, not brightness). Blob positions rehash every 12
    // frames so mass builds up locally before moving on; matter is injected
    // at mid-level (≈0.6) which sits in the growth band's feeding range.
    if (p.injectAmount > 0.0f) {
        float2 uv = (float2(gid) + 0.5f) / float2(p.width, p.height);
        float aspect = float(p.width) / float(p.height);
        uint cycle = p.frameIndex / 12u;
        float blobR = float(p.radius) * 0.9f / float(p.height);
        for (uint i = 0; i < p.injectCount; ++i) {
            float2 c = float2(rand01(uint2(i, cycle), 811u, p.seed),
                              rand01(uint2(cycle, i), 823u, p.seed));
            float2 d = uv - c;
            d = d - round(d); // toroidal
            d.x *= aspect;
            float r2 = dot(d, d) / (blobR * blobR);
            float bump = exp(-r2 * 0.5f);
            A2 = mix(A2, 0.6f, clamp(bump * p.injectAmount, 0.0f, 1.0f));
        }
    }

    dst.write(float4(clamp(A2, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f), gid);
}
