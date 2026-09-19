// Shaders/Particle/Tracers.metal
// Passive tracer particles: no neighbour interaction, advected by an
// externally-coupled flow field plus jitter, respawning biased toward
// bright emitField regions (e.g. a coupled SlimeMold trail). Companion to
// SlimeMold.metal — every Shaders/*.metal file is concatenated into ONE
// library at runtime, so every symbol here is prefixed `tracer(s)` to stay
// globally unique. Matches life::TracersModule::TracersParams (scalar-packed,
// same field order).

struct TracersParams {
    uint particleCount;
    float dt;             // seconds
    float lifetime;        // s, respawned life = lifetime * (0.5 + rand)
    float emitBias;         // probability of trying emitField-biased respawn
    float emitThreshold;   // emitField value a respawn candidate must beat
    float flowWeight;      // how strongly flowField advects a particle
    float jitter;           // px/s, random-direction speed added every frame
    uint wallY;            // strip world: floor/ceiling are walls. 0 = torus
    uint seed;
    uint frameIndex;
    uint width;
    uint height;
};

// Per-particle PCG stream, thread-local. Same construction as SlimeMold's
// slimeNextRand but its own name (see file header on symbol uniqueness).
static float tracerNextRand(thread uint& state) {
    state = pcg_hash(state);
    return float(state >> 8) * (1.0f / 16777216.0f);
}

// Per-particle depth in [0,1), re-derived from id every kernel invocation
// (not stored) so tracersMove and tracersSplat always agree on it.
static float tracerDepth(uint id) {
    return float(pcg_hash(id ^ 0x54524143u) >> 8) * (1.0f / 16777216.0f); // "TRAC"
}

kernel void tracersInit(device float2* positions [[buffer(0)]],
                        device float2* ageLife [[buffer(1)]],
                        device uint* randomState [[buffer(2)]],
                        constant TracersParams& p [[buffer(3)]],
                        uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;

    float w = float(p.width);
    float h = float(p.height);
    uint state = pcg_hash(id ^ p.seed);

    float2 pos = float2(tracerNextRand(state) * w, tracerNextRand(state) * h);
    float life = p.lifetime * (0.5f + tracerNextRand(state));
    float age = tracerNextRand(state) * life; // spread respawns over time

    positions[id] = pos;
    ageLife[id] = float2(age, life);
    randomState[id] = state;
}

kernel void tracersClearDensity(device atomic_uint* density [[buffer(0)]],
                                constant TracersParams& p [[buffer(1)]],
                                uint id [[thread_position_in_grid]]) {
    if (id >= p.width * p.height) return;
    atomic_store_explicit(&density[id], 0u, memory_order_relaxed);
}

kernel void tracersMove(device float2* positions [[buffer(0)]],
                        device float2* ageLife [[buffer(1)]],
                        device uint* randomState [[buffer(2)]],
                        texture2d<float, access::sample> flow [[texture(0)]],
                        texture2d<float, access::sample> emit [[texture(1)]],
                        constant TracersParams& p [[buffer(3)]],
                        uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;

    float w = float(p.width);
    float h = float(p.height);
    float2 pos = positions[id];
    float2 al = ageLife[id];
    float age = al.x;
    float life = al.y;
    uint state = pcg_hash(randomState[id] ^ p.frameIndex);

    float z = tracerDepth(id);
    float speedScale = mix(0.45f, 1.0f, z);

    float2 flowVel = sampleFieldWrap4(flow, edgePos(pos, h, p.wallY), w, h).xy;
    float angle = tracerNextRand(state) * 6.28318530718f;
    float2 jitterVel = float2(cos(angle), sin(angle)) * p.jitter;
    float2 vel = flowVel * p.flowWeight * speedScale + jitterVel;

    pos += vel * p.dt;
    pos.x = fract(pos.x / w) * w;
    if (p.wallY != 0u) {
        // Bounce off floor/ceiling (matches slimeMove).
        if (pos.y < 0.0f) pos.y = -pos.y;
        else if (pos.y >= h) pos.y = 2.0f * h - pos.y;
        pos.y = clamp(pos.y, 0.0f, h - 0.001f);
    } else {
        pos.y = fract(pos.y / h) * h;
    }
    age += p.dt;

    if (age >= life) {
        float2 newPos;
        bool found = false;
        if (tracerNextRand(state) < p.emitBias) {
            for (int k = 0; k < 8; ++k) {
                float2 cand = float2(tracerNextRand(state) * w, tracerNextRand(state) * h);
                if (sampleFieldWrap(emit, cand, w, h) > p.emitThreshold) {
                    newPos = cand;
                    found = true;
                    break;
                }
            }
        }
        if (!found) newPos = float2(tracerNextRand(state) * w, tracerNextRand(state) * h);
        pos = newPos;
        life = p.lifetime * (0.5f + tracerNextRand(state));
        age = 0.0f;
    }

    positions[id] = pos;
    ageLife[id] = float2(age, life);
    randomState[id] = state;
}

// ---- Render: density splat + soft-saturated resolve --------------------
// Matches life::TracersModule::TracersSplatParams / TracersResolveParams
// (scalar-packed). Same fixed-point (scale 256) atomic_uint accumulation
// scheme as ParticleSplat.metal's splatAccumulate/splatResolve — integer
// adds are order-independent, so this stays deterministic regardless of GPU
// thread scheduling.

struct TracersSplatParams {
    uint particleCount;
    uint width;
    uint height;
    uint wallY;
    float bokehRadius; // px, only the far z>0.98 slice draws this large
};

kernel void tracersSplat(device const float2* positions [[buffer(0)]],
                         device const float2* ageLife [[buffer(1)]],
                         device atomic_uint* density [[buffer(2)]],
                         constant TracersSplatParams& p [[buffer(3)]],
                         uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;

    float2 pos = positions[id];
    float2 al = ageLife[id];
    float age = al.x;
    float life = al.y;

    float z = tracerDepth(id);
    float brightness = mix(0.2f, 1.0f, z * z);

    // Smooth fade-in over the first 0.5s, fade-out over the last 30% of life.
    float fadeIn = smoothstep(0.0f, 0.5f, age);
    float fadeOut = 1.0f - smoothstep(max(life * 0.7f, 0.0f), max(life, 0.0001f), age);
    float weight = brightness * fadeIn * fadeOut;
    if (weight <= 0.0f) return;

    if (z > 0.98f) {
        // Nearest 2%: soft out-of-focus disc, not a brighter point — spread
        // the same total weight over the disc area (x3 so it still reads).
        float radius = mix(1.5f, p.bokehRadius, (z - 0.98f) / 0.02f);
        float area = 3.14159265359f * radius * radius;
        float perPixel = weight / max(area, 1.0f) * 3.0f;
        uint add = uint(perPixel * 256.0f);
        if (add == 0u) return;

        int ir = int(ceil(radius));
        int2 base = int2(floor(pos));
        for (int dy = -ir; dy <= ir; ++dy) {
            int py = base.y + dy;
            if (p.wallY != 0u && (py < 0 || py >= int(p.height))) continue;
            for (int dx = -ir; dx <= ir; ++dx) {
                int px = base.x + dx;
                float2 d = (float2(px, py) + 0.5f - pos) / radius;
                if (dot(d, d) >= 1.0f) continue;
                uint2 q = wrapCoord(int2(px, py), p.width, p.height);
                uint idx = q.y * p.width + q.x;
                atomic_fetch_add_explicit(&density[idx], add, memory_order_relaxed);
            }
        }
    } else {
        uint2 cell = uint2(min(uint(pos.x), p.width - 1u), min(uint(pos.y), p.height - 1u));
        uint idx = cell.y * p.width + cell.x;
        atomic_fetch_add_explicit(&density[idx], uint(weight * 256.0f), memory_order_relaxed);
    }
}

struct TracersResolveParams {
    uint width;
    uint height;
    float gain;
    float density;   // soft-saturation rate: v = gain * (1 - exp(-d*density))
    float tintR, tintG, tintB;
    float edgeFade;  // px, floor/ceiling fade (same shape as the relief's)
};

kernel void tracersResolve(device atomic_uint* density [[buffer(0)]],
                           texture2d<float, access::write> out [[texture(0)]],
                           constant TracersResolveParams& p [[buffer(1)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    uint idx = gid.y * p.width + gid.x;
    float d = float(atomic_load_explicit(&density[idx], memory_order_relaxed)) / 256.0f;
    atomic_store_explicit(&density[idx], 0u, memory_order_relaxed);

    float v = p.gain * (1.0f - exp(-d * p.density));
    if (p.edgeFade > 0.0f) {
        float fade = smoothstep(0.0f, p.edgeFade,
                                min(float(gid.y), float(p.height - 1u) - float(gid.y)));
        v *= fade;
    }
    float3 rgb = float3(p.tintR, p.tintG, p.tintB) * v;
    out.write(float4(rgb, saturate(v)), gid);
}
