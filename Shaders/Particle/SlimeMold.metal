// Shaders/Particle/SlimeMold.metal
// Physarum transport-network agents (Jeff Jones, "Characteristics of Pattern
// Formation and Evolution in Approximations of Physarum Transport Networks",
// Artificial Life 2010 — design doc §12.4). Agents sense a shared pheromone
// trail field, steer toward the strongest signal, move, and deposit; the
// trail then diffuses and decays. Matches life::SlimeMoldModule::SlimeParams
// (scalar-packed, same field order).

struct SlimeParams {
    uint agentCount;
    float dt;             // seconds
    float moveSpeed;      // px/s
    float sensorAngle;    // rad (one side)
    float sensorBoost;    // snare -> sensorAngle multiplier boost (0..1)
    float sensorDistance; // px
    float turnSpeed;      // rad/s
    float turnImpulse;    // perc -> extra random-turn probability strength
    float jitter;         // hihat -> directional noise
    float depositAmount;  // amount one agent lays down per step
    float resetPulse;     // beat -> respawn gate probability
    float decayRate;      // trail decay (1/s), trail *= exp(-decayRate*dt)
    float diffuseRate;    // 0..1: blend fraction toward the 3x3 mean
    uint spawnMode;       // 0 uniform, 1 filled circle, 2 ring
    uint seed;
    uint frameIndex;
    uint width;
    uint height;
    float attractorWeight; // Phase 5: weight of attractorField in the sensor read
    float flowWeight;      // Phase 12: how strongly flowField advects agents
    uint wallY;            // strip world: floor/ceiling are walls (x stays toroidal). 0 = torus
};

// Advances a per-agent PCG stream held in thread-local state and returns the
// next value in [0,1) — same bit-scale as rand01(), but evolving frame to
// frame so slimeMove doesn't need to re-derive a fresh hash tag per draw.
// The stream's origin (randomState[id] at init) is pcg_hash(id ^ seed), so
// the whole trajectory stays seed-derived and fully deterministic.
static float slimeNextRand(thread uint& state) {
    state = pcg_hash(state);
    return float(state >> 8) * (1.0f / 16777216.0f);
}

// spawnMode: 0 uniform, 1 filled circle, 2 ring (both centered on the field).
// Used for in-flight respawns (slimeMove's resetPulse branch); slimeInit
// uses its own rand01-based version so the id-keyed initial placement stays
// independent of however many draws a respawn elsewhere might consume.
static float2 slimeSpawnPosition(uint spawnMode, float w, float h, thread uint& state) {
    float2 center = float2(w, h) * 0.5f;
    if (spawnMode == 1u) {
        float r = sqrt(slimeNextRand(state)) * min(w, h) * 0.4f;
        float a = slimeNextRand(state) * 6.28318530718f;
        return center + float2(cos(a), sin(a)) * r;
    } else if (spawnMode == 2u) {
        float r = min(w, h) * 0.35f;
        float a = slimeNextRand(state) * 6.28318530718f;
        return center + float2(cos(a), sin(a)) * r;
    }
    return float2(slimeNextRand(state) * w, slimeNextRand(state) * h);
}

// One sensor read: trail blended with the coupled attractor (Phase 5 §3a).
// In a walled strip a sensor poking past floor/ceiling reads 0 — nothing to
// follow out there — which also keeps agents from piling up along the wall.
static float slimeSense(texture2d<float, access::sample> trail,
                        texture2d<float, access::sample> attractor,
                        float2 q, float w, float h, constant SlimeParams& p) {
    if (p.wallY != 0u && (q.y < 0.0f || q.y >= h)) return 0.0f;
    return sampleFieldWrap(trail, q, w, h) +
           p.attractorWeight * sampleFieldWrap(attractor, q, w, h);
}

kernel void slimeInit(device float2* positions [[buffer(0)]],
                      device float2* headings [[buffer(1)]],
                      device uint* randomState [[buffer(2)]],
                      constant SlimeParams& p [[buffer(3)]],
                      uint id [[thread_position_in_grid]]) {
    if (id >= p.agentCount) return;

    float w = float(p.width);
    float h = float(p.height);
    float2 center = float2(w, h) * 0.5f;
    float2 pos;

    if (p.spawnMode == 1u) {
        // filled circle
        float r = sqrt(rand01(uint2(id, 101u), 101u, p.seed)) * min(w, h) * 0.4f;
        float a = rand01(uint2(id, 103u), 103u, p.seed) * 6.28318530718f;
        pos = center + float2(cos(a), sin(a)) * r;
    } else if (p.spawnMode == 2u) {
        // ring
        float r = min(w, h) * 0.35f;
        float a = rand01(uint2(id, 107u), 107u, p.seed) * 6.28318530718f;
        pos = center + float2(cos(a), sin(a)) * r;
    } else {
        // uniform
        pos = float2(rand01(uint2(id, 109u), 109u, p.seed) * w,
                     rand01(uint2(id, 113u), 113u, p.seed) * h);
    }

    float headingAngle = rand01(uint2(id, 127u), 127u, p.seed) * 6.28318530718f;

    positions[id] = pos;
    headings[id] = float2(cos(headingAngle), sin(headingAngle));
    randomState[id] = pcg_hash(id ^ p.seed);
}

kernel void slimeClearTrail(texture2d<float, access::write> trail [[texture(0)]],
                            constant SlimeParams& p [[buffer(0)]],
                            uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    trail.write(float4(0.0f, 0.0f, 0.0f, 0.0f), gid);
}

kernel void slimeClearDeposit(device atomic_uint* deposit [[buffer(0)]],
                              constant SlimeParams& p [[buffer(1)]],
                              uint id [[thread_position_in_grid]]) {
    if (id >= p.width * p.height) return;
    atomic_store_explicit(&deposit[id], 0u, memory_order_relaxed);
}

kernel void slimeMove(device float2* positions [[buffer(0)]],
                      device float2* headings [[buffer(1)]],
                      device uint* randomState [[buffer(2)]],
                      device atomic_uint* deposit [[buffer(3)]],
                      texture2d<float, access::sample> trail [[texture(0)]],
                      texture2d<float, access::sample> attractor [[texture(1)]],
                      texture2d<float, access::sample> flow [[texture(2)]],
                      constant SlimeParams& p [[buffer(4)]],
                      constant AudioUniforms& audio [[buffer(5)]],
                      uint id [[thread_position_in_grid]]) {
    if (id >= p.agentCount) return;

    float w = float(p.width);
    float h = float(p.height);
    float2 pos = positions[id];
    float2 heading = headings[id];
    // Fold the frame counter into the persistent per-agent stream (same
    // spirit as the field modules' `audio.frameIndex`-tagged rand01 calls);
    // still fully seed-derived since randomState[id] originates from
    // pcg_hash(id ^ seed) in slimeInit.
    uint state = pcg_hash(randomState[id] ^ audio.frameIndex);

    float sensorAngleEff = p.sensorAngle * (1.0f + p.sensorBoost);
    float baseAngle = atan2(heading.y, heading.x);

    float2 dirF = float2(cos(baseAngle), sin(baseAngle));
    float2 dirL = float2(cos(baseAngle + sensorAngleEff), sin(baseAngle + sensorAngleEff));
    float2 dirR = float2(cos(baseAngle - sensorAngleEff), sin(baseAngle - sensorAngleEff));

    // Phase 5 §3a: sensed value blends the trail with an externally-coupled
    // attractor field (e.g. a Lenia density), weighted by attractorWeight.
    // attractor is a 4x4 black fallback when no scene connection targets
    // "attractorField", so this is a no-op (sense == trail-only) by default.
    float F = slimeSense(trail, attractor, pos + dirF * p.sensorDistance, w, h, p);
    float L = slimeSense(trail, attractor, pos + dirL * p.sensorDistance, w, h, p);
    float R = slimeSense(trail, attractor, pos + dirR * p.sensorDistance, w, h, p);

    // Jones 2010 standard steer: go straight when the front sensor leads,
    // turn toward the stronger side sensor, and pick randomly when the
    // front is weaker than BOTH sides (no directional signal to follow).
    float turn = p.turnSpeed * p.dt;
    if (F >= L && F >= R) {
        // straight
    } else if (F < L && F < R) {
        baseAngle += (slimeNextRand(state) < 0.5f ? -1.0f : 1.0f) * turn;
    } else if (L > R) {
        baseAngle += turn;
    } else if (R > L) {
        baseAngle -= turn;
    }

    // hihat -> directional noise.
    baseAngle += (slimeNextRand(state) * 2.0f - 1.0f) * p.jitter * p.dt;

    // perc -> hard random re-direction impulse.
    if (slimeNextRand(state) < p.turnImpulse * 0.05f) {
        baseAngle = slimeNextRand(state) * 6.28318530718f;
    }

    heading = float2(cos(baseAngle), sin(baseAngle));

    // beat -> respawn gate.
    if (slimeNextRand(state) < p.resetPulse * 0.02f) {
        pos = slimeSpawnPosition(p.spawnMode, w, h, state);
        float a = slimeNextRand(state) * 6.28318530718f;
        heading = float2(cos(a), sin(a));
    }

    // Phase 12: external flow (e.g. fluid velocity) carries the agent;
    // heading is untouched so trail-following continues while drifting.
    float2 drift = (p.flowWeight != 0.0f)
        ? sampleFieldWrap4(flow, edgePos(pos, h, p.wallY), w, h).xy * p.flowWeight
        : float2(0.0f, 0.0f);
    pos += (heading * p.moveSpeed + drift) * p.dt;
    pos.x = fract(pos.x / w) * w;
    if (p.wallY != 0u) {
        // Bounce off floor/ceiling.
        if (pos.y < 0.0f) { pos.y = -pos.y; heading.y = -heading.y; }
        else if (pos.y >= h) { pos.y = 2.0f * h - pos.y; heading.y = -heading.y; }
        pos.y = clamp(pos.y, 0.0f, h - 0.001f);
    } else {
        pos.y = fract(pos.y / h) * h;
    }

    uint2 cell = uint2(min(uint(pos.x), p.width - 1u), min(uint(pos.y), p.height - 1u));
    uint idx = cell.y * p.width + cell.x;
    atomic_fetch_add_explicit(&deposit[idx], uint(p.depositAmount * 1024.0f),
                              memory_order_relaxed);

    positions[id] = pos;
    headings[id] = heading;
    randomState[id] = state;
}

kernel void slimeTrailUpdate(texture2d<float, access::read> trailIn [[texture(0)]],
                             texture2d<float, access::write> trailOut [[texture(1)]],
                             device uint* deposit [[buffer(0)]],
                             constant SlimeParams& p [[buffer(1)]],
                             uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 base = int2(gid);
    float sum = 0.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            uint2 q = edgeCoord(base + int2(dx, dy), p.width, p.height, p.wallY);
            sum += trailIn.read(q).x;
        }
    }
    float mean = sum / 9.0f;
    float center = trailIn.read(gid).x;

    float v = mix(center, mean, p.diffuseRate) * exp(-p.decayRate * p.dt);

    uint idx = gid.y * p.width + gid.x;
    v += float(deposit[idx]) / 1024.0f;
    deposit[idx] = 0u; // self cell only; plain write is race-free here

    trailOut.write(float4(v, 0.0f, 0.0f, 0.0f), gid);
}

// ---- Beads: every beadStride-th agent drawn as a small lit sphere straight
// onto the module's output layer (after colorMap/relief). They ride the
// agents, so they stream along the bodies and drift alone through the dark.
// Beads cluster densely on organism heads and overlap constantly there, so a
// single read-modify-write kernel (one thread per bead, racing directly on
// the output texture) is order-dependent — different overlap orders produce
// different final pixels, which is what actually made room-b-sculpt.json
// non-deterministic run to run at scale (not the Fluid<->Slime coupling;
// that only mattered because it made beads overlap more). Fixed with a
// three-pass claim/draw/release trio sharing ONE atomic_uint width*height
// buffer (the SAME one showAgents/strayGain use, via
// ParticleSplatPass::ensureDensity — SlimeMoldModule::encode leaves it
// all-zero again before either of those runs their own accumulate):
//   1. slimeBeadsClaim  — every covered pixel keeps the MAX of a per-bead
//      key (edge coverage, then sphere normal z, then bead id) via a CAS
//      loop — deterministic regardless of thread scheduling.
//   2. slimeBeadsDraw   — each bead re-checks the key; only the pixel's
//      final winner writes (exactly one writer per pixel -> race-free).
//   3. slimeBeadsRelease — every covered pixel is reset to 0 (all writers
//      store the same value, so this is race-free too).
// Only 16 bits of the key identify the bead (SlimeMoldModule::encode clamps
// the bead count to 65536, raising the effective stride if needed), so all
// three kernels dispatch1D over BEAD count, not agent count. Geometry/
// shading is factored into slimeBead*() helpers so the three kernels can't
// drift apart. Matches life::SlimeMoldModule::BeadParams (scalar-packed,
// unchanged).
struct SlimeBeadParams {
    uint agentCount;
    uint stride;
    uint width;
    uint height;
    uint wallY;
    float radius;      // px; per-bead size is 0.5x..1.5x of this
    float lightX, lightY, lightZ;
    float ambient;
    float diffuse;
    float specular;
    float shininess;
    float tintR, tintG, tintB;
    float exposure;
    float edgeFade;    // px: same floor/ceiling fade as the relief, so beads don't line the edges
};

// Per-bead center + jittered radius + floor/ceiling fade. Returns false when
// the bead is fully faded out (skip it entirely, in all three passes).
static bool slimeBeadGeometry(uint agent, device const float2* positions,
                              constant SlimeBeadParams& p,
                              thread float2& c, thread float& r, thread float& fade) {
    c = positions[agent];
    r = p.radius * (0.5f + float(pcg_hash(agent ^ 0x42454144u) >> 8) * (1.0f / 16777216.0f));
    fade = 1.0f;
    if (p.edgeFade > 0.0f) {
        fade = smoothstep(0.0f, p.edgeFade, min(c.y, float(p.height) - c.y));
        if (fade <= 0.0f) return false;
    }
    return true;
}

// Per-pixel coverage test against one bead's sphere + the ~1px soft rim
// weight (edge, already including the floor/ceiling fade). False means the
// pixel is outside the bead (or the rim is too faint to matter — matches
// the >=1/255 floor slimeBeadsClaim/Draw/Release all skip below).
static bool slimeBeadCoverage(int2 q, float2 c, float r, float fade,
                              thread float2& nxy, thread float& d2, thread float& edge) {
    float2 d = (float2(q) + 0.5f - c) / r;
    d2 = dot(d, d);
    if (d2 >= 1.0f) return false;
    nxy = d;
    edge = saturate((1.0f - sqrt(d2)) * r) * fade;
    return uint(edge * 255.0f) >= 1u;
}

// Blinn-Phong shade for a pixel already known to be covered (nxy/d2 from
// slimeBeadCoverage above).
static float slimeBeadShade(float2 nxy, float d2, constant SlimeBeadParams& p) {
    float3 n = float3(nxy, sqrt(1.0f - d2));
    float3 L = float3(p.lightX, p.lightY, p.lightZ);
    float3 H = normalize(L + float3(0.0f, 0.0f, 1.0f));
    return p.ambient + p.diffuse * max(dot(n, L), 0.0f) +
           p.specular * pow(max(dot(n, H), 0.0f), p.shininess);
}

// Shared iteration bounds: every pixel a bead's sphere could possibly cover.
static void slimeBeadBounds(float2 c, float r, thread int2& base, thread int& ir) {
    ir = int(ceil(r)) + 1;
    base = int2(floor(c));
}

kernel void slimeBeadsClaim(device const float2* positions [[buffer(0)]],
                            device atomic_uint* claim [[buffer(1)]],
                            constant SlimeBeadParams& p [[buffer(2)]],
                            uint id [[thread_position_in_grid]]) {
    uint agent = id * p.stride;
    if (agent >= p.agentCount) return;

    float2 c; float r; float fade;
    if (!slimeBeadGeometry(agent, positions, p, c, r, fade)) return;

    int2 base; int ir;
    slimeBeadBounds(c, r, base, ir);
    for (int dy = -ir; dy <= ir; ++dy) {
        for (int dx = -ir; dx <= ir; ++dx) {
            int2 q = base + int2(dx, dy);
            if (p.wallY != 0u && (q.y < 0 || q.y >= int(p.height))) continue;
            float2 nxy; float d2; float edge;
            if (!slimeBeadCoverage(q, c, r, fade, nxy, d2, edge)) continue;

            uint e = uint(edge * 255.0f);
            uint nz = uint(saturate(sqrt(1.0f - d2)) * 255.0f);
            uint key = (e << 24) | (nz << 16) | (id & 0xFFFFu);

            uint2 w = wrapCoord(q, p.width, p.height);
            uint idx = w.y * p.width + w.x;
            uint old = atomic_load_explicit(&claim[idx], memory_order_relaxed);
            while (key > old &&
                   !atomic_compare_exchange_weak_explicit(&claim[idx], &old, key,
                                                          memory_order_relaxed,
                                                          memory_order_relaxed)) {
            }
        }
    }
}

kernel void slimeBeadsDraw(device const float2* positions [[buffer(0)]],
                           device atomic_uint* claim [[buffer(1)]],
                           constant SlimeBeadParams& p [[buffer(2)]],
                           texture2d<float, access::read_write> out [[texture(0)]],
                           uint id [[thread_position_in_grid]]) {
    uint agent = id * p.stride;
    if (agent >= p.agentCount) return;

    float2 c; float r; float fade;
    if (!slimeBeadGeometry(agent, positions, p, c, r, fade)) return;

    float3 tint = float3(p.tintR, p.tintG, p.tintB) * p.exposure;
    int2 base; int ir;
    slimeBeadBounds(c, r, base, ir);
    for (int dy = -ir; dy <= ir; ++dy) {
        for (int dx = -ir; dx <= ir; ++dx) {
            int2 q = base + int2(dx, dy);
            if (p.wallY != 0u && (q.y < 0 || q.y >= int(p.height))) continue;
            float2 nxy; float d2; float edge;
            if (!slimeBeadCoverage(q, c, r, fade, nxy, d2, edge)) continue;

            uint2 w = wrapCoord(q, p.width, p.height);
            uint idx = w.y * p.width + w.x;
            uint key = atomic_load_explicit(&claim[idx], memory_order_relaxed);
            if ((key & 0xFFFFu) != (id & 0xFFFFu)) continue; // another bead won this pixel

            float v = slimeBeadShade(nxy, d2, p);
            float4 cur = out.read(w);
            out.write(float4(mix(cur.rgb, v * tint, edge), max(cur.a, edge)), w);
        }
    }
}

kernel void slimeBeadsRelease(device const float2* positions [[buffer(0)]],
                              device atomic_uint* claim [[buffer(1)]],
                              constant SlimeBeadParams& p [[buffer(2)]],
                              uint id [[thread_position_in_grid]]) {
    uint agent = id * p.stride;
    if (agent >= p.agentCount) return;

    float2 c; float r; float fade;
    if (!slimeBeadGeometry(agent, positions, p, c, r, fade)) return;

    int2 base; int ir;
    slimeBeadBounds(c, r, base, ir);
    for (int dy = -ir; dy <= ir; ++dy) {
        for (int dx = -ir; dx <= ir; ++dx) {
            int2 q = base + int2(dx, dy);
            if (p.wallY != 0u && (q.y < 0 || q.y >= int(p.height))) continue;
            float2 nxy; float d2; float edge;
            if (!slimeBeadCoverage(q, c, r, fade, nxy, d2, edge)) continue;

            uint2 w = wrapCoord(q, p.width, p.height);
            uint idx = w.y * p.width + w.x;
            atomic_store_explicit(&claim[idx], 0u, memory_order_relaxed); // every writer stores 0 -> race-free
        }
    }
}

// ---- Strays: a separate, soft-saturated overlay of ALL agents' raw
// density (not just showAgents' gated splat), masked to show only where the
// trail itself is faint — i.e. lone agents wandering in the dark, not the
// thousands-per-pixel interior of an organism (showAgents blows that out to
// white; this stays dark there via the mask). The count itself comes from
// the generic splatAccumulate kernel (ParticleSplat.metal) accumulating
// into the SAME density buffer showAgents' ParticleSplatPass already owns
// (SlimeMoldModule::encode reuses it via ParticleSplatPass::ensureDensity)
// — this is only the resolve. Matches life::SlimeMoldModule::
// StrayResolveParams (scalar-packed).
struct StrayResolveParams {
    uint width;
    uint height;
    float strayGain;
    float strayDensity;
    float strayMaskLo;
    float strayMaskHi;
    float tintR, tintG, tintB;
    float exposure;
    float edgeFade;
};

kernel void strayResolve(device atomic_uint* density [[buffer(0)]],
                         texture2d<float, access::read> trail [[texture(0)]],
                         texture2d<float, access::read_write> out [[texture(1)]],
                         constant StrayResolveParams& p [[buffer(1)]],
                         uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    uint idx = gid.y * p.width + gid.x;
    float d = float(atomic_load_explicit(&density[idx], memory_order_relaxed)) / 256.0f;
    atomic_store_explicit(&density[idx], 0u, memory_order_relaxed); // self cell only, race-free

    float v = p.strayGain * (1.0f - exp(-d * p.strayDensity));

    // Every agent deposits on its own pixel (~1-3 for a lone agent, tens to
    // hundreds inside organisms), so the mask thresholds the trail value
    // itself rather than some derived agent count.
    float trailValue = trail.read(gid).x;
    float mask = 1.0f - smoothstep(p.strayMaskLo, p.strayMaskHi, trailValue);
    v *= mask;

    if (p.edgeFade > 0.0f) {
        float fade = smoothstep(0.0f, p.edgeFade,
                                min(float(gid.y), float(p.height - 1u) - float(gid.y)));
        v *= fade;
    }

    float3 tint = float3(p.tintR, p.tintG, p.tintB) * p.exposure;
    float4 cur = out.read(gid);
    out.write(float4(cur.rgb + v * tint, cur.a), gid);
}
