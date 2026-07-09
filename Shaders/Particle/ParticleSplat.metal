// Shaders/Particle/ParticleSplat.metal
// Reusable particle→texture resolve (design doc §8.4): every particle system
// must end in an RGBA16F texture before Composite. Density accumulates in a
// uint fixed-point buffer (scale 256) via atomic adds — integer addition is
// order-independent, so accumulation stays deterministic regardless of GPU
// thread scheduling (no float atomics, per the suite-wide constraint).

struct SplatParams {
    uint particleCount;
    uint width;
    uint height;
    float weight;
    float gainR;
    float gainG;
    float gainB;
    float gain;
};

kernel void splatClear(device atomic_uint* density [[buffer(0)]],
                       constant SplatParams& p [[buffer(1)]],
                       uint id [[thread_position_in_grid]]) {
    if (id >= p.width * p.height) return;
    atomic_store_explicit(&density[id], 0u, memory_order_relaxed);
}

kernel void splatAccumulate(device const float2* positions [[buffer(0)]],
                            device atomic_uint* density [[buffer(1)]],
                            constant SplatParams& p [[buffer(2)]],
                            uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;

    int2 cell = int2(floor(positions[id]));
    uint2 c = wrapCoord(cell, p.width, p.height);
    uint idx = c.y * p.width + c.x;
    atomic_fetch_add_explicit(&density[idx], uint(p.weight * 256.0f),
                              memory_order_relaxed);
}

kernel void splatResolve(device atomic_uint* density [[buffer(0)]],
                         texture2d<float, access::read_write> target [[texture(0)]],
                         constant SplatParams& p [[buffer(1)]],
                         uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    uint idx = gid.y * p.width + gid.x;
    float d = float(atomic_load_explicit(&density[idx], memory_order_relaxed)) / 256.0f;

    float4 cur = target.read(gid);
    float3 add = float3(p.gainR, p.gainG, p.gainB) * d * p.gain;
    target.write(float4(cur.rgb + add, cur.a), gid);

    // Self-cell only, so a plain reset here is race-free; splatClear is only
    // needed once, before the first accumulate.
    atomic_store_explicit(&density[idx], 0u, memory_order_relaxed);
}
