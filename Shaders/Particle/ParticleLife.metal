// Shaders/Particle/ParticleLife.metal
// Particle Life (design doc §12.5, after Tom Mohr's particle-life framework):
// K species, a K×K signed interaction matrix, universal short-range
// repulsion, spatial-hash neighbor search. Positions/velocities are
// ping-ponged — plStep reads the whole previous state (posR/velR) and writes
// the next (posW/velW); in-place update would race and break determinism.
// Neighbor iteration follows the grid's canonical (index-ascending) order, so
// the float force accumulation order is fixed → bit-deterministic per seed.
// Matches life::ParticleLifeModule::PLParams (scalar-packed, same order).

struct PLParams {
    uint particleCount;
    uint speciesCount;
    float dt;
    float rMax;       // interaction radius == grid cellSize
    float beta;       // repulsion/attraction crossover (normalized r)
    float forceScale;
    float friction;   // 1/s, vel *= exp(-friction*dt)
    float maxSpeed;   // px/s
    float jitter;     // hihat -> micro jitter (px/s²-ish, × dt onto vel)
    float forceBoost; // kick -> attraction matrix boost
    uint seed;
    uint frameIndex;
    float worldW;
    float worldH;
    float cellSize;
    uint cellsX;
    uint cellsY;
    float fieldForce; // Phase 5: gain on the forceField gradient force
};

// Forward declaration: defined in ParticleSplat.metal, which is concatenated
// AFTER this file (sources are merged in path order into one translation
// unit, so declaring here and defining later is fine).
inline void splatAddRGB(device atomic_uint* rgb, uint cellIdx, float3 c);

// Per-particle persistent PCG stream (same scheme as slimeNextRand; each
// module keeps its own copy because the shader library is one concatenated
// translation unit and helper names must be unique).
static float plNextRand(thread uint& state) {
    state = pcg_hash(state);
    return float(state >> 8) * (1.0f / 16777216.0f);
}

kernel void plInit(device float2* posW [[buffer(0)]],
                   device float2* velW [[buffer(1)]],
                   device uint* species [[buffer(2)]],
                   device uint* randomState [[buffer(3)]],
                   constant PLParams& p [[buffer(4)]],
                   uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;
    posW[id] = float2(rand01(uint2(id, 11u), 11u, p.seed) * p.worldW,
                      rand01(uint2(id, 13u), 13u, p.seed) * p.worldH);
    velW[id] = float2(0.0f, 0.0f);
    species[id] = min(uint(rand01(uint2(id, 17u), 17u, p.seed) *
                           float(p.speciesCount)),
                      p.speciesCount - 1u);
    randomState[id] = pcg_hash(id ^ p.seed);
}

kernel void plStep(texture2d<float, access::sample> field [[texture(0)]],
                   device const float2* posR [[buffer(0)]],
                   device const float2* velR [[buffer(1)]],
                   device float2* posW [[buffer(2)]],
                   device float2* velW [[buffer(3)]],
                   device const uint* species [[buffer(4)]],
                   device uint* randomState [[buffer(5)]],
                   device const uint* cellStart [[buffer(6)]],
                   device const uint* cellCount [[buffer(7)]],
                   device const uint* sorted [[buffer(8)]],
                   device const float* interactionMatrix [[buffer(9)]],
                   constant PLParams& p [[buffer(10)]],
                   constant AudioUniforms& audio [[buffer(11)]],
                   uint i [[thread_position_in_grid]]) {
    if (i >= p.particleCount) return;

    float2 world = float2(p.worldW, p.worldH);
    float2 pi = posR[i];
    uint si = species[i];

    // 3×3 neighbor cells around our own (positions are wrapped, so pi ≥ 0;
    // min-clamp mirrors hashCount's cell assignment at the float edge).
    int2 cc = min(int2(pi / p.cellSize),
                  int2(int(p.cellsX) - 1, int(p.cellsY) - 1));

    float2 force = float2(0.0f, 0.0f);
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
                if (r < 1e-5f || r >= p.rMax) continue;

                // Standard Particle Life force profile: universal repulsion
                // below beta, matrix-signed triangular bump between beta
                // and 1 (zero at both ends).
                float rn = r / p.rMax;
                float f;
                if (rn < p.beta) {
                    f = rn / p.beta - 1.0f; // negative → repulsion
                } else {
                    float a = interactionMatrix[si * p.speciesCount + species[j]] *
                              (1.0f + p.forceBoost);
                    f = a * (1.0f - fabs(2.0f * rn - 1.0f - p.beta) /
                                        (1.0f - p.beta));
                }
                force += (d / r) * f;
            }
        }
    }

    // Phase 5 §3c: central-difference gradient of a coupled field adds a
    // force (e.g. climb/descend a Reaction-Diffusion concentration). field
    // is a 4x4 black fallback when unconnected, so this is a no-op by
    // default (flat field -> zero gradient).
    if (p.fieldForce != 0.0f) {
        float e = 2.0f;
        float gx = sampleFieldWrap(field, pi + float2(e, 0.0f), p.worldW, p.worldH) -
                   sampleFieldWrap(field, pi - float2(e, 0.0f), p.worldW, p.worldH);
        float gy = sampleFieldWrap(field, pi + float2(0.0f, e), p.worldW, p.worldH) -
                   sampleFieldWrap(field, pi - float2(0.0f, e), p.worldW, p.worldH);
        force += float2(gx, gy) * p.fieldForce;
    }

    float2 vel = velR[i] + force * p.forceScale * p.dt;
    vel *= exp(-p.friction * p.dt);
    float len = length(vel);
    if (len > p.maxSpeed) vel *= p.maxSpeed / len;

    if (p.jitter > 0.0f) {
        // Persistent per-particle stream folded with the frame counter (same
        // spirit as slimeMove); origin pcg_hash(id ^ seed) → seed-derived.
        uint state = pcg_hash(randomState[i] ^ audio.frameIndex);
        float a = plNextRand(state) * 6.28318530718f;
        vel += float2(cos(a), sin(a)) * p.jitter * p.dt;
        randomState[i] = state;
    }

    float2 pos = pi + vel * p.dt;
    pos = fract(pos / world) * world; // toroidal wrap (negative-safe)

    posW[i] = pos;
    velW[i] = vel;
}

// Splat each particle's species color into the shared RGB fixed-point buffer
// (splatAddRGB / splatResolveRGB live in ParticleSplat.metal). speciesColor
// is K×4 floats (rgb + pad), CPU-generated cosine palette.
kernel void plSplatAccum(device const float2* posR [[buffer(0)]],
                         device const uint* species [[buffer(1)]],
                         device const float* speciesColor [[buffer(2)]],
                         device atomic_uint* rgb [[buffer(3)]],
                         constant PLParams& p [[buffer(4)]],
                         uint id [[thread_position_in_grid]]) {
    if (id >= p.particleCount) return;

    uint w = uint(p.worldW);
    uint h = uint(p.worldH);
    uint2 c = wrapCoord(int2(floor(posR[id])), w, h);
    uint s = species[id];
    float3 col = float3(speciesColor[s * 4u + 0u], speciesColor[s * 4u + 1u],
                        speciesColor[s * 4u + 2u]);
    splatAddRGB(rgb, c.y * w + c.x, col);
}
