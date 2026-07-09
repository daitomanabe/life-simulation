// Shaders/Field/CellularAutomata.metal
// Discrete CA rules on an R16F field, toroidal. Cell encoding:
//   Life/Seeds:    0 dead, 1 alive
//   Brian's Brain: 0 off, 0.5 refractory, 1 firing
//   Cyclic:        state/N in [0,1)

struct CAParams {
    uint rule; // 0 life, 1 brain, 2 seeds, 3 cyclic
    uint cyclicStates;
    float injectAmount;
    float initDensity;
    uint seed;
    uint width;
    uint height;
};

kernel void caInit(texture2d<float, access::write> state [[texture(0)]],
                   constant CAParams& p [[buffer(0)]],
                   uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    float r = rand01(gid, 5u, p.seed);
    float v = 0.0f;
    if (p.rule == 3u) {
        // Cyclic CA starts from uniform random states.
        uint s = uint(rand01(gid, 9u, p.seed) * float(p.cyclicStates));
        v = float(s) / float(p.cyclicStates);
    } else {
        v = r < p.initDensity ? 1.0f : 0.0f;
    }
    state.write(float4(v, 0, 0, 0), gid);
}

kernel void caStep(texture2d<float, access::read> src [[texture(0)]],
                   texture2d<float, access::write> dst [[texture(1)]],
                   constant CAParams& p [[buffer(0)]],
                   constant AudioUniforms& audio [[buffer(1)]],
                   uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 base = int2(gid);
    float self = src.read(gid).x;
    float next = 0.0f;

    if (p.rule == 3u) {
        // Cyclic CA: advance if any Moore neighbor holds state+1 (mod N).
        float N = float(p.cyclicStates);
        uint s = uint(round(self * N));
        uint target = (s + 1u) % p.cyclicStates;
        bool advance = false;
        for (int dy = -1; dy <= 1 && !advance; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                uint2 q = wrapCoord(base + int2(dx, dy), p.width, p.height);
                uint ns = uint(round(src.read(q).x * N)) % p.cyclicStates;
                if (ns == target) { advance = true; break; }
            }
        }
        next = float(advance ? target : s) / N;
    } else {
        // Count firing neighbors (value ≥ 0.75 counts as alive/firing).
        int alive = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                uint2 q = wrapCoord(base + int2(dx, dy), p.width, p.height);
                if (src.read(q).x >= 0.75f) alive++;
            }
        }
        if (p.rule == 0u) { // Conway B3/S23
            bool live = self >= 0.75f;
            next = (live ? (alive == 2 || alive == 3) : (alive == 3)) ? 1.0f : 0.0f;
        } else if (p.rule == 1u) { // Brian's Brain B2, firing→refractory→off
            if (self >= 0.75f) next = 0.5f;
            else if (self >= 0.25f) next = 0.0f;
            else next = (alive == 2) ? 1.0f : 0.0f;
        } else { // Seeds B2/S(none)
            next = (self < 0.75f && alive == 2) ? 1.0f : 0.0f;
        }
    }

    // Audio-driven random cell injection (hihat → sparkle).
    if (p.injectAmount > 0.0f) {
        float r = rand01(gid, 907u + audio.frameIndex % 1021u, p.seed);
        if (r > 1.0f - p.injectAmount * 0.002f) next = 1.0f;
    }

    dst.write(float4(next, 0, 0, 0), gid);
}
