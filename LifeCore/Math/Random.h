#pragma once
// LifeCore/Math/Random.h
// CPU-side deterministic random utilities. All randomness in the suite must be
// derived from an explicit seed so Realtime and Offline runs are reproducible.

#include <cstdint>

namespace life {

// SplitMix64: fast, high-quality 64-bit stream. Used to derive sub-seeds.
struct SplitMix64 {
    uint64_t state = 0x9E3779B97F4A7C15ull;

    explicit SplitMix64(uint64_t seed = 0) : state(seed + 0x9E3779B97F4A7C15ull) {}

    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // [0, 1)
    float nextFloat01() {
        return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f);
    }

    float nextRange(float lo, float hi) { return lo + (hi - lo) * nextFloat01(); }

    uint32_t nextUint32() { return static_cast<uint32_t>(next() >> 32); }
};

// PCG-style 32-bit hash (matches Shaders/Common/Random.metal pcg_hash).
inline uint32_t pcgHash(uint32_t v) {
    uint32_t state = v * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Derive a per-purpose sub-seed from a global seed and a purpose tag, so each
// module / pass gets an independent deterministic stream.
inline uint32_t deriveSeed(uint32_t globalSeed, uint32_t purposeTag) {
    return pcgHash(globalSeed ^ pcgHash(purposeTag));
}

} // namespace life
