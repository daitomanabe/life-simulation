// Shaders/Common/Random.metal
// Deterministic GPU hashing / random. Same PCG constants as
// LifeCore/Math/Random.h so CPU and GPU streams can be cross-checked.

inline uint pcg_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

inline uint hash_combine(uint a, uint b) {
    return pcg_hash(a ^ (b + 0x9E3779B9u + (a << 6u) + (a >> 2u)));
}

// Per-pixel, per-tag deterministic float in [0, 1).
inline float rand01(uint2 pixel, uint tag, uint seed) {
    uint h = pcg_hash(pixel.x ^ pcg_hash(pixel.y ^ pcg_hash(tag ^ seed)));
    return float(h >> 8) * (1.0f / 16777216.0f);
}

inline float2 rand2(uint2 pixel, uint tag, uint seed) {
    return float2(rand01(pixel, tag * 2u + 0u, seed),
                  rand01(pixel, tag * 2u + 1u, seed));
}
