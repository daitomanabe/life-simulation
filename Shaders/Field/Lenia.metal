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

// ---- phase9 (docs/specs/phase9_lenia_fft.md §3-4): FFT-convolution growth
// pass. leniaInit/leniaStep above are UNCHANGED (backward-compat contract);
// this is a wholly new kernel used only when convMode="fft". It reads up to
// 4 precomputed potentials (one FFT convolution per kernel, done in
// Modules/FieldModules/Lenia.cpp before this pass) and folds them into a
// single growth update. Mirrors life::LeniaModule::LeniaMultiParams
// (this exact order) — deliberately separate from LeniaParams above.

struct LeniaMultiParams {
    float dt;
    float kMu[4];
    float kSigma[4];
    float kWeight[4];
    uint numKernels;
    uint growthMode; // 0 standard (2·bell−1 weighted sum), 1 asymptotic (weighted bell average − A)
    float noiseAmount;
    float muJitter;
    float injectAmount;
    uint injectCount;
    uint radius;
    uint seed;
    uint width;
    uint height;
    uint frameIndex;
};

static inline float leniaBell(float u, float mu, float sigma) {
    float t = (u - mu) / sigma;
    return exp(-0.5f * t * t);
}

kernel void leniaGrowthMulti(texture2d<float, access::read> src [[texture(0)]],
                             texture2d<float, access::write> dst [[texture(1)]],
                             texture2d<float, access::read> potential0 [[texture(2)]],
                             texture2d<float, access::read> potential1 [[texture(3)]],
                             texture2d<float, access::read> potential2 [[texture(4)]],
                             texture2d<float, access::read> potential3 [[texture(5)]],
                             constant LeniaMultiParams& p [[buffer(0)]],
                             constant AudioUniforms& audio [[buffer(1)]],
                             uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float A = src.read(gid).x;

    // muJitter shifts every kernel's growth center by the same per-pixel
    // amount (topology break) — copied from leniaStep's formula verbatim.
    float muJ = 0.0f;
    if (p.muJitter > 0.0f) {
        float j = rand01(gid, 401u + audio.frameIndex % 613u, p.seed) - 0.5f;
        muJ = j * p.muJitter * 0.05f;
    }

    float bell[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (p.numKernels > 0u) bell[0] = leniaBell(potential0.read(gid).x, p.kMu[0] + muJ, p.kSigma[0]);
    if (p.numKernels > 1u) bell[1] = leniaBell(potential1.read(gid).x, p.kMu[1] + muJ, p.kSigma[1]);
    if (p.numKernels > 2u) bell[2] = leniaBell(potential2.read(gid).x, p.kMu[2] + muJ, p.kSigma[2]);
    if (p.numKernels > 3u) bell[3] = leniaBell(potential3.read(gid).x, p.kMu[3] + muJ, p.kSigma[3]);

    float sumWeighted = 0.0f; // Σ h_k · bell_k
    float sumWeights = 0.0f;  // Σ h_k
    float sumStd = 0.0f;      // Σ h_k · (2·bell_k − 1)
    for (uint k = 0; k < p.numKernels && k < 4u; ++k) {
        sumWeighted += p.kWeight[k] * bell[k];
        sumWeights += p.kWeight[k];
        sumStd += p.kWeight[k] * (2.0f * bell[k] - 1.0f);
    }

    float A2;
    if (p.growthMode == 1u) {
        float target = (sumWeights > 1e-6f) ? (sumWeighted / sumWeights) : A;
        A2 = A + p.dt * (target - A);
    } else {
        A2 = A + p.dt * sumStd;
    }

    // noiseAmount / injectAmount blob injection — copied from leniaStep
    // verbatim (direct-path behavior parity, §3-4 "既存の...ロジックを
    // leniaStep からコピーして適用"; leniaStep itself is not touched so
    // that its own md5 contract is untouched).
    if (p.noiseAmount > 0.0f) {
        float n = rand01(gid, 503u, p.seed) - 0.5f;
        A2 += n * p.noiseAmount * 0.03f;
    }

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

// ---- phase11 (docs/specs/phase11_organisms.md): organism stamp init.
// leniaStampInit places up to 32 copies ("stamps") of a known organism's
// cell pattern (loaded from Presets/organisms/*.json, uploaded as an R32F
// texture by Modules/FieldModules/Lenia.cpp) into the field at random
// positions/orientations, instead of leniaInit's noise soup. leniaInit
// itself is UNCHANGED above (backward-compat contract, phase9 comment).
// Mirrors life::LeniaModule::LeniaStampParams (this exact order).

struct LeniaStampParams {
    uint width;
    uint height;
    uint orgWidth;
    uint orgHeight;
    uint stampCount;
    uint stampRotate; // 0/1 (bool banned in uniform structs, §constraint 2)
    uint seed;
};

// Maps a world-space offset `d` (a stamp center -> gid delta, already
// toroidally unwrapped) to the corresponding integer-preserving offset in
// the organism's own unrotated local frame, for one of 8 dihedral
// (rotate 0/90/180/270 x flip-x) orientations. Every branch is an axis
// swap/negation, so integer pixel offsets stay exact — nearest-sampling
// (spec: "バイリニアでなく nearest でよい") needs no interpolation.
static inline float2 leniaStampInverse(float2 d, uint variant) {
    if ((variant & 4u) != 0u) d.x = -d.x;
    uint rot = variant & 3u;
    if (rot == 1u) d = float2(d.y, -d.x);
    else if (rot == 2u) d = float2(-d.x, -d.y);
    else if (rot == 3u) d = float2(-d.y, d.x);
    return d;
}

kernel void leniaStampInit(texture2d<float, access::write> state [[texture(0)]],
                           texture2d<float, access::read> organism [[texture(1)]],
                           constant LeniaStampParams& p [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float A = 0.0f;
    float2 orgCenter = float2(p.orgWidth, p.orgHeight) * 0.5f;
    uint count = min(p.stampCount, 32u);
    for (uint i = 0; i < count; ++i) {
        // Stamp i's position/orientation are pure hashes of (i, seed), so
        // every pixel's loop iteration agrees on the same stamp centers
        // without any extra buffer/pass (same trick leniaStep's
        // injectAmount blobs already use for their per-cycle centers).
        float2 c = float2(rand01(uint2(i, 0u), 907u, p.seed),
                          rand01(uint2(0u, i), 911u, p.seed)) * float2(p.width, p.height);

        uint variant = 0u;
        if (p.stampRotate != 0u) {
            variant = min(uint(rand01(uint2(i, 1u), 919u, p.seed) * 8.0f), 7u);
        }

        float2 d = float2(gid) + 0.5f - c;
        d.x -= float(p.width) * round(d.x / float(p.width));   // toroidal
        d.y -= float(p.height) * round(d.y / float(p.height));

        float2 local = leniaStampInverse(d, variant) + orgCenter;
        int lx = int(floor(local.x));
        int ly = int(floor(local.y));
        if (lx >= 0 && lx < int(p.orgWidth) && ly >= 0 && ly < int(p.orgHeight)) {
            float v = organism.read(uint2(uint(lx), uint(ly))).x;
            A = max(A, v);
        }
    }

    state.write(float4(clamp(A, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f), gid);
}
