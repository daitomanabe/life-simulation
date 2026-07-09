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
    float F = sampleFieldWrap(trail, pos + dirF * p.sensorDistance, w, h) +
              p.attractorWeight * sampleFieldWrap(attractor, pos + dirF * p.sensorDistance, w, h);
    float L = sampleFieldWrap(trail, pos + dirL * p.sensorDistance, w, h) +
              p.attractorWeight * sampleFieldWrap(attractor, pos + dirL * p.sensorDistance, w, h);
    float R = sampleFieldWrap(trail, pos + dirR * p.sensorDistance, w, h) +
              p.attractorWeight * sampleFieldWrap(attractor, pos + dirR * p.sensorDistance, w, h);

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
        ? sampleFieldWrap4(flow, pos, w, h).xy * p.flowWeight
        : float2(0.0f, 0.0f);
    pos += (heading * p.moveSpeed + drift) * p.dt;
    pos.x = fract(pos.x / w) * w;
    pos.y = fract(pos.y / h) * h;

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
            uint2 q = wrapCoord(base + int2(dx, dy), p.width, p.height);
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
