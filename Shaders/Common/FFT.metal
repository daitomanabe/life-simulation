// Shaders/Common/FFT.metal
// Phase 9 (docs/specs/phase9_lenia_fft.md §2): self-contained radix-2
// Stockham autosort FFT for 2D real-valued fields (Lenia FFT convolution).
// No MPSGraph/Accelerate — a self-rolled compute kernel keeps every pass
// inside CommandGraph's per-pass GPU timing and determinism story (spec
// §3.7 note: "MPS に全部任せない").
//
// One dispatch == one Stockham stage; the C++ side (Modules/FieldModules/
// Lenia.cpp, static encodeFFT2D helper) drives the log2(N) stages, ping-
// ponging between two RG32F (re, im) textures. Complex data is always
// RG32F (R = real, G = imaginary).
//
// ---- Algorithm ----
// Radix-2 Stockham autosort (Govindaraju et al. 2008, "High Performance
// Discrete Fourier Transforms on Graphics Processors" — the canonical
// GPU-FFT autosort formulation, generalized here to R=2):
//
//   for stage s with span Ns = 2^s (s = 0 .. log2(N)-1), thread j in
//   [0, N/2):
//     v0 = data[j]                                  (fixed half-stride read)
//     v1 = data[j + N/2]
//     angle = -dir * 2*PI * (j mod Ns) / (Ns*2)
//     v1   *= (cos(angle), sin(angle))               (twiddle; r=0 tap is untouched)
//     out0  = v0 + v1                                (radix-2 butterfly)
//     out1  = v0 - v1
//     idxD  = (j / Ns)*Ns*2 + (j mod Ns)             (autosort scatter — no bit-reversal pass needed)
//     data'[idxD] = out0;  data'[idxD + Ns] = out1
//
// forward: dir = +1, no scale anywhere. inverse: dir = -1, and the LAST
// stage of each 1D direction (X then Y) scales its own output by 1/N. Since
// (1/W)*(1/H) = 1/(W*H), doing this once per axis reproduces the single
// 1/(W*H) 2D inverse-DFT normalization exactly — verified numerically by
// the phase9 roundtrip self-test (see final report: max abs error < 1e-4
// on a 256x128 random real field, forward -> inverse -> compare).
//
// This formula was hand-verified against a direct N=4 DFT by the
// implementer before writing this file (X[0..3] match term-for-term), then
// machine-verified by the roundtrip test — see final report for both.
//
// 2D transform = separable 1D transforms: fftStageX runs one stage of the
// row-wise (X, length = width) transform for every row in parallel;
// fftStageY runs one stage of the column-wise (Y, length = height)
// transform for every column, after all X stages are done. Twiddles are
// computed with cos/sin per invocation — no LUT texture, sizes are a
// runtime choice (simWidth/simHeight).

#include <metal_stdlib>
using namespace metal;

struct FFTParams {
    uint width;
    uint height;
    uint N;         // transform length for this call (width for an X-stage, height for a Y-stage)
    uint Ns;        // current stage span (1, 2, 4, ... N/2)
    int  dir;       // +1 forward, -1 inverse
    uint normalize; // 1 => scale this stage's output by 1/N (inverse's final stage only)
};

inline float2 fftComplexMul(float2 a, float2 b) {
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// One radix-2 Stockham autosort stage along X (row-wise), every row done in
// parallel. gid.x in [0, width/2); gid.y in [0, height).
kernel void fftStageX(texture2d<float, access::read> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      constant FFTParams& p [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    uint halfN = p.N / 2u;
    if (gid.x >= halfN || gid.y >= p.height) return;

    uint j = gid.x;
    uint Ns = p.Ns;
    float angle = -float(p.dir) * 6.28318530718f * float(j % Ns) / float(Ns * 2u);
    float2 tw = float2(cos(angle), sin(angle));

    float2 v0 = src.read(uint2(j, gid.y)).xy;
    float2 v1 = fftComplexMul(src.read(uint2(j + halfN, gid.y)).xy, tw);

    float2 out0 = v0 + v1;
    float2 out1 = v0 - v1;
    if (p.normalize != 0u) {
        float inv = 1.0f / float(p.N);
        out0 *= inv;
        out1 *= inv;
    }

    uint idxD = (j / Ns) * Ns * 2u + (j % Ns);
    dst.write(float4(out0, 0.0f, 0.0f), uint2(idxD, gid.y));
    dst.write(float4(out1, 0.0f, 0.0f), uint2(idxD + Ns, gid.y));
}

// One radix-2 Stockham autosort stage along Y (column-wise), every column
// done in parallel. gid.x in [0, width); gid.y in [0, height/2).
kernel void fftStageY(texture2d<float, access::read> src [[texture(0)]],
                      texture2d<float, access::write> dst [[texture(1)]],
                      constant FFTParams& p [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    uint halfN = p.N / 2u;
    if (gid.x >= p.width || gid.y >= halfN) return;

    uint j = gid.y;
    uint Ns = p.Ns;
    float angle = -float(p.dir) * 6.28318530718f * float(j % Ns) / float(Ns * 2u);
    float2 tw = float2(cos(angle), sin(angle));

    float2 v0 = src.read(uint2(gid.x, j)).xy;
    float2 v1 = fftComplexMul(src.read(uint2(gid.x, j + halfN)).xy, tw);

    float2 out0 = v0 + v1;
    float2 out1 = v0 - v1;
    if (p.normalize != 0u) {
        float inv = 1.0f / float(p.N);
        out0 *= inv;
        out1 *= inv;
    }

    uint idxD = (j / Ns) * Ns * 2u + (j % Ns);
    dst.write(float4(out0, 0.0f, 0.0f), uint2(gid.x, idxD));
    dst.write(float4(out1, 0.0f, 0.0f), uint2(gid.x, idxD + Ns));
}

// dst = a * b (pointwise complex multiply — the convolution theorem step),
// optionally scaled by 1/(width*height) when p.normalize != 0. Lenia's
// convolution path (Modules/FieldModules/Lenia.cpp) always calls this with
// normalize=0 and instead lets the *inverse* FFT2D's own per-stage
// normalize do the 1/(W*H) scaling (single code path, already verified by
// the roundtrip test) — the flag exists here too so the op is meaningful
// standalone, per spec §2's "scale は params 経由 or normalize フラグで
// 1/(W*H)".
kernel void complexMulScale(texture2d<float, access::read> a [[texture(0)]],
                            texture2d<float, access::read> b [[texture(1)]],
                            texture2d<float, access::write> dst [[texture(2)]],
                            constant FFTParams& p [[buffer(0)]],
                            uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    float2 va = a.read(gid).xy;
    float2 vb = b.read(gid).xy;
    float2 r = fftComplexMul(va, vb);
    if (p.normalize != 0u) r *= 1.0f / float(p.width * p.height);
    dst.write(float4(r, 0.0f, 0.0f), gid);
}

// R32F -> RG32F (im = 0).
kernel void realToComplex(texture2d<float, access::read> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant FFTParams& p [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    float re = src.read(gid).x;
    dst.write(float4(re, 0.0f, 0.0f, 0.0f), gid);
}

// RG32F -> R32F: takes the real part (the imaginary part should be ~0 here
// up to float error — Lenia only ever runs this on the inverse FFT of a
// product of two real-valued fields' spectra, which is conjugate-symmetric).
kernel void complexToReal(texture2d<float, access::read> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant FFTParams& p [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    float re = src.read(gid).x;
    dst.write(float4(re, 0.0f, 0.0f, 0.0f), gid);
}

// Zero-fills an R32F texture. Used once (needsInit) to clear all 4
// multi-kernel potential slots so leniaGrowthMulti (Shaders/Field/Lenia.metal)
// never reads uninitialized GPU memory for kernel indices >= numKernels —
// those slots' kWeight is 0 so a *finite* stale/zero value there contributes
// exactly nothing to the growth sum (NaN would not — this kernel guarantees
// finite).
kernel void clearR32F(texture2d<float, access::write> dst [[texture(0)]],
                      constant FFTParams& p [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    dst.write(float4(0.0f), gid);
}
