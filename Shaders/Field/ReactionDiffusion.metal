// Shaders/Field/ReactionDiffusion.metal
// Gray-Scott reaction diffusion. State: RG32F, x = A (substrate), y = B
// (activator). 9-point laplacian, wrap boundary, audio-modulated rules:
//   feed        — already audio-modulated on CPU (kick → birth rate)
//   killPerturb — spatial kill perturbation (snare → topology break)
//   noiseAmount — B-channel jitter (hihat → diffusion noise)

struct RDParams {
    float Du;
    float Dv;
    float feed;
    float kill;
    float dt;
    float noiseAmount;
    float killPerturb;
    uint seed;
    uint initSpots;
    float initSpotRadius;
    uint width;
    uint height;
};

kernel void rdInit(texture2d<float, access::write> state [[texture(0)]],
                   constant RDParams& p [[buffer(0)]],
                   uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float2 uv = float2(gid) / float2(p.width, p.height);
    float b = 0.0f;

    // Deterministic circular seed spots (one centered + initSpots hashed).
    float aspect = float(p.width) / float(p.height);
    {
        float2 d = uv - float2(0.5f, 0.5f);
        d.x *= aspect;
        if (length(d) < p.initSpotRadius) b = 1.0f;
    }
    for (uint i = 0; i < p.initSpots; ++i) {
        float2 c = float2(rand01(uint2(i, 17u), 101u, p.seed),
                          rand01(uint2(i, 43u), 202u, p.seed));
        float2 d = uv - c;
        d.x *= aspect;
        if (length(d) < p.initSpotRadius) b = 1.0f;
    }

    state.write(float4(1.0f, b, 0.0f, 0.0f), gid);
}

kernel void rdStep(texture2d<float, access::read> src [[texture(0)]],
                   texture2d<float, access::write> dst [[texture(1)]],
                   constant RDParams& p [[buffer(0)]],
                   constant AudioUniforms& audio [[buffer(1)]],
                   uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 ip = int2(gid);
    float2 c = src.read(gid).xy;

    // 9-point laplacian, toroidal.
    float2 lap = -c;
    lap += 0.2f * src.read(wrapCoord(ip + int2(1, 0), p.width, p.height)).xy;
    lap += 0.2f * src.read(wrapCoord(ip + int2(-1, 0), p.width, p.height)).xy;
    lap += 0.2f * src.read(wrapCoord(ip + int2(0, 1), p.width, p.height)).xy;
    lap += 0.2f * src.read(wrapCoord(ip + int2(0, -1), p.width, p.height)).xy;
    lap += 0.05f * src.read(wrapCoord(ip + int2(1, 1), p.width, p.height)).xy;
    lap += 0.05f * src.read(wrapCoord(ip + int2(-1, 1), p.width, p.height)).xy;
    lap += 0.05f * src.read(wrapCoord(ip + int2(1, -1), p.width, p.height)).xy;
    lap += 0.05f * src.read(wrapCoord(ip + int2(-1, -1), p.width, p.height)).xy;

    float A = c.x;
    float B = c.y;

    // Rule-space audio modulation (design doc §1.2): the SYSTEM changes, not
    // just the pixels. killPerturb warps the kill rate spatially; noiseAmount
    // injects micro-jitter into the activator.
    float killLocal = p.kill;
    if (p.killPerturb > 0.0f) {
        float n = rand01(gid, 7001u + audio.frameIndex % 977u, p.seed);
        killLocal += p.killPerturb * (n - 0.5f) * 0.08f;
    }

    float reaction = A * B * B;
    float A2 = A + (p.Du * lap.x - reaction + p.feed * (1.0f - A)) * p.dt;
    float B2 = B + (p.Dv * lap.y + reaction - (killLocal + p.feed) * B) * p.dt;

    if (p.noiseAmount > 0.0f) {
        float n = rand01(gid, 9103u, p.seed) - 0.5f;
        B2 += n * p.noiseAmount * 0.05f;
    }

    dst.write(float4(clamp(A2, 0.0f, 1.5f), clamp(B2, 0.0f, 1.5f), 0.0f, 0.0f), gid);
}
