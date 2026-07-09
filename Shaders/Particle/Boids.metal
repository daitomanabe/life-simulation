// Shaders/Particle/Boids.metal
// Boids flocking (design doc §12.6, Craig Reynolds "Flocks, Herds, and
// Schools" 1987): separation / alignment / cohesion over the spatial-hash
// 3×3 neighborhood. Positions/velocities are ping-ponged (read previous
// state, write next) and neighbors iterate in the grid's canonical
// ascending-index order, so accumulation is bit-deterministic per seed.
// Matches life::BoidsModule::BoidsParams (scalar-packed, same order).

struct BoidsParams {
    uint particleCount;
    float dt;
    float radius;      // neighbor radius == grid cellSize
    float sepWeight;
    float aliWeight;
    float cohWeight;
    float minSpeed;    // px/s
    float maxSpeed;    // px/s
    float jitter;      // hihat -> local jitter
    float sepBoost;    // kick -> separation burst
    float impulse;     // perc -> random direction impulse
    float scatterPulse; // snare.trigger -> scatter (velocity randomize)
    uint seed;
    uint frameIndex;
    float worldW;
    float worldH;
    float cellSize;
    uint cellsX;
    uint cellsY;
    float sepRadiusFrac; // separation acts only within radius*this (<1)
    float flowWeight;    // Phase 8: weight of the flowField steering force
};

// Forward declaration: defined in ParticleSplat.metal, which is concatenated
// AFTER this file (sources are merged in path order into one translation
// unit, so declaring here and defining later is fine).
inline void splatAddRGB(device atomic_uint* rgb, uint cellIdx, float3 c);

// Per-particle persistent PCG stream (unique name per module — the shader
// library is one concatenated translation unit).
static float boidsNextRand(thread uint& state) {
    state = pcg_hash(state);
    return float(state >> 8) * (1.0f / 16777216.0f);
}

kernel void boidsInit(device float2* posW [[buffer(0)]],
                      device float2* velW [[buffer(1)]],
                      device uint* randomState [[buffer(2)]],
                      constant BoidsParams& p [[buffer(3)]],
                      uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;
    posW[id] = float2(rand01(uint2(id, 19u), 19u, p.seed) * p.worldW,
                      rand01(uint2(id, 23u), 23u, p.seed) * p.worldH);
    float a = rand01(uint2(id, 29u), 29u, p.seed) * 6.28318530718f;
    velW[id] = float2(cos(a), sin(a)) * (p.minSpeed + p.maxSpeed) * 0.5f;
    randomState[id] = pcg_hash(id ^ p.seed);
}

kernel void boidsStep(texture2d<float, access::sample> flowField [[texture(0)]],
                      device const float2* posR [[buffer(0)]],
                      device const float2* velR [[buffer(1)]],
                      device float2* posW [[buffer(2)]],
                      device float2* velW [[buffer(3)]],
                      device uint* randomState [[buffer(4)]],
                      device const uint* cellStart [[buffer(5)]],
                      device const uint* cellCount [[buffer(6)]],
                      device const uint* sorted [[buffer(7)]],
                      constant BoidsParams& p [[buffer(8)]],
                      constant AudioUniforms& audio [[buffer(9)]],
                      uint i [[thread_position_in_grid]]) {
    if (i >= p.particleCount) return;

    float2 world = float2(p.worldW, p.worldH);
    float2 pi = posR[i];
    float2 vel = velR[i];
    // Persistent per-particle stream folded with the frame counter (same
    // spirit as slimeMove); origin pcg_hash(id ^ seed) → seed-derived.
    uint state = pcg_hash(randomState[i] ^ audio.frameIndex);

    // 3×3 neighbor cells around our own (positions are wrapped, so pi ≥ 0;
    // min-clamp mirrors hashCount's cell assignment at the float edge).
    int2 cc = min(int2(pi / p.cellSize),
                  int2(int(p.cellsX) - 1, int(p.cellsY) - 1));

    float2 sep = float2(0.0f, 0.0f);
    float2 aliSum = float2(0.0f, 0.0f);
    float2 cohSum = float2(0.0f, 0.0f);
    uint count = 0u;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = (cc.x + dx + int(p.cellsX)) % int(p.cellsX);
            int cy = (cc.y + dy + int(p.cellsY)) % int(p.cellsY);
            uint cell = uint(cy) * p.cellsX + uint(cx);
            uint start = cellStart[cell];
            uint n = cellCount[cell];
            for (uint k = start; k < start + n; ++k) {
                uint j = sorted[k];
                if (j == i) continue;
                float2 d = posR[j] - pi;
                d -= world * round(d / world); // minimum image (toroidal)
                float r = length(d);
                if (r < 1e-5f || r >= p.radius) continue;
                // Separation only at short range — full-radius separation
                // cancels cohesion and collapses flocking into a uniform
                // aligned gas (observed in review).
                float sepR = p.radius * p.sepRadiusFrac;
                if (r < sepR) sep += -d / r * (1.0f - r / sepR);
                aliSum += velR[j];
                cohSum += pi + d; // neighbor position in i's minimum image
                count++;
            }
        }
    }

    float2 acc = sep * p.sepWeight * (1.0f + p.sepBoost);
    if (count > 0u) {
        float inv = 1.0f / float(count);
        acc += (aliSum * inv - vel) * p.aliWeight;
        acc += (cohSum * inv - pi) * p.cohWeight;
    }

    // Fluid coupling (Phase 8 §3): flowField steers velocity toward the
    // sampled fluid velocity, the same shape as the alignment term above.
    // flowGain is fixed at 1.0 — the Fluid module already builds its
    // velocity field in px/s, the same units as vel here. flowWeight
    // defaults to 0 (and the port falls back to a black/zero texture when
    // unconnected), so this is a true no-op for every scene that doesn't
    // opt in.
    if (p.flowWeight != 0.0f) {
        float2 flow = sampleFieldWrap4(flowField, pi, p.worldW, p.worldH).xy;
        acc += (flow - vel) * p.flowWeight;
    }

    // perc -> random direction impulse.
    if (p.impulse > 0.0f && boidsNextRand(state) < p.impulse * 0.03f) {
        float a = boidsNextRand(state) * 6.28318530718f;
        acc += float2(cos(a), sin(a)) * p.maxSpeed * 4.0f;
    }

    // snare.trigger -> scatter: replace velocity with a random direction.
    if (p.scatterPulse > 0.0f && boidsNextRand(state) < p.scatterPulse * 0.08f) {
        float a = boidsNextRand(state) * 6.28318530718f;
        vel = float2(cos(a), sin(a)) * p.maxSpeed;
    }

    vel += acc * p.dt;

    // hihat -> local jitter.
    if (p.jitter > 0.0f) {
        float a = boidsNextRand(state) * 6.28318530718f;
        vel += float2(cos(a), sin(a)) * p.jitter * p.dt;
    }

    // Clamp speed to [minSpeed, maxSpeed]; re-ignite dead boids.
    float len = length(vel);
    if (len < 1e-4f) {
        float a = boidsNextRand(state) * 6.28318530718f;
        vel = float2(cos(a), sin(a)) * p.minSpeed;
    } else if (len < p.minSpeed) {
        vel *= p.minSpeed / len;
    } else if (len > p.maxSpeed) {
        vel *= p.maxSpeed / len;
    }

    float2 pos = pi + vel * p.dt;
    pos = fract(pos / world) * world; // toroidal wrap (negative-safe)

    posW[i] = pos;
    velW[i] = vel;
    randomState[i] = state;
}

// Splat each boid colored by heading hue (cosine palette over atan2 angle)
// into the shared RGB fixed-point buffer (resolve = splatResolveRGB in
// ParticleSplat.metal).
kernel void boidsSplatAccum(device const float2* posR [[buffer(0)]],
                            device const float2* velR [[buffer(1)]],
                            device atomic_uint* rgb [[buffer(2)]],
                            constant BoidsParams& p [[buffer(3)]],
                            uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;

    float2 v = velR[id];
    float hue = atan2(v.y, v.x) / 6.28318530718f;
    float3 col =
        0.5f + 0.5f * cos(6.28318530718f * (hue + float3(0.0f, 0.33f, 0.67f)));

    uint w = uint(p.worldW);
    uint h = uint(p.worldH);
    uint2 c = wrapCoord(int2(floor(posR[id])), w, h);
    splatAddRGB(rgb, c.y * w + c.x, col);
}
