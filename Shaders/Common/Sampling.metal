// Shaders/Common/Sampling.metal
// FieldSampler: bilinear, wrap-addressed field sampling for particle sensors
// (Slime Mold trail sensing) and any future particle→field reads (design doc
// §9.4 / Phase 3 FieldSampler). Common/ so it is visible to every later
// .metal file once concatenated.

// posPx: pixel-space position (NOT normalized). The repeat-address sampler
// makes the read toroidal to match the wrap boundary the rest of the suite
// uses (§7.5) — the caller never needs to pre-wrap posPx, even for negative
// or out-of-range values.
inline float sampleFieldWrap(texture2d<float, access::sample> t, float2 posPx,
                             float w, float h) {
    constexpr sampler s(filter::linear, address::repeat, coord::normalized);
    return t.sample(s, posPx / float2(w, h)).x;
}
