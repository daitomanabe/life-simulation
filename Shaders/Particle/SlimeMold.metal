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
// One thread per bead, ~(2r)^2 px each — tens of thousands of beads cost
// nothing next to the full-screen passes. Overlapping beads race on the
// write; both write near-identical bright values, so it is not visible.
// Matches life::SlimeMoldModule::BeadParams (scalar-packed).
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

kernel void slimeBeads(device const float2* positions [[buffer(0)]],
                       texture2d<float, access::read_write> out [[texture(0)]],
                       constant SlimeBeadParams& p [[buffer(1)]],
                       uint id [[thread_position_in_grid]]) {
    uint agent = id * p.stride;
    if (agent >= p.agentCount) return;

    float2 c = positions[agent];
    float r = p.radius * (0.5f + float(pcg_hash(agent ^ 0x42454144u) >> 8) * (1.0f / 16777216.0f));
    float3 L = float3(p.lightX, p.lightY, p.lightZ);
    float3 H = normalize(L + float3(0.0f, 0.0f, 1.0f));
    float3 tint = float3(p.tintR, p.tintG, p.tintB) * p.exposure;
    float fade = 1.0f;
    if (p.edgeFade > 0.0f) {
        fade = smoothstep(0.0f, p.edgeFade, min(c.y, float(p.height) - c.y));
        if (fade <= 0.0f) return;
    }

    int ir = int(ceil(r)) + 1;
    int2 base = int2(floor(c));
    for (int dy = -ir; dy <= ir; ++dy) {
        for (int dx = -ir; dx <= ir; ++dx) {
            int2 q = base + int2(dx, dy);
            if (p.wallY != 0u && (q.y < 0 || q.y >= int(p.height))) continue;
            float2 d = (float2(q) + 0.5f - c) / r;
            float d2 = dot(d, d);
            if (d2 >= 1.0f) continue;
            float3 n = float3(d, sqrt(1.0f - d2));
            float v = p.ambient + p.diffuse * max(dot(n, L), 0.0f) +
                      p.specular * pow(max(dot(n, H), 0.0f), p.shininess);
            float edge = saturate((1.0f - sqrt(d2)) * r) * fade; // ~1 px soft rim
            uint2 w = wrapCoord(q, p.width, p.height);
            float4 cur = out.read(w);
            out.write(float4(mix(cur.rgb, v * tint, edge), max(cur.a, edge)), w);
        }
    }
}
