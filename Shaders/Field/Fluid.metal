// Shaders/Field/Fluid.metal
// Stable Fluids (Stam 1999 / GPU Gems ch.38): semi-Lagrangian advection +
// Jacobi-relaxed pressure projection, over a velocity field (RG32F) and a
// colored dye field (RGBA16F) that IS the visual. Toroidal (wrap) boundary
// by default — no obstacles, so every pass is a plain wrapCoord / repeat-
// sampler read (design doc §7.5). wallY turns floor/ceiling into walls for a
// strip world (edgeCoord / edgePos, x still wraps). Matches
// life::FluidModule::FluidParams (scalar-packed, same order).

struct FluidParams {
    uint width;
    uint height;
    float dt;
    float velDissipation;  // velocity retention/s (vel *= exp(-velDissipation*dt))
    float dyeDissipation;  // dye retention/s
    float vorticity;       // confinement epsilon (0 = off, 2-6 typical)
    float impulse;         // kick -> velocity+dye impulse strength
    float impulseRadius;   // px
    float turbulence;      // hihat -> small-scale random force
    float injectHue;       // base hue for injected dye (beat rotates it via mapping)
    float low;              // AudioFeatureState.low, transcribed on CPU each frame
    float mid;               // AudioFeatureState.mid
    float high;              // AudioFeatureState.high
    uint seed;
    uint frameIndex;
    float dyeInject; // dye replacement fraction at impulse center (velocity-independent)
    float forceFieldGain; // Phase 12: gradient force from the bound forceField
    uint wallY;   // strip world: floor/ceiling are walls (x stays toroidal). 0 = torus
    float driftX; // px/s^2 along x, sin profile in y (0 at floor/ceiling, max mid-height)
};

// ---- fluidPresent's own tiny uniform (Phase 8 §1: dye -> output is a plain
// exposure copy, not a ColorMapPass — dye is already color). Named distinctly
// from Shaders/Render/Present.metal's PresentParams (same shape, different
// struct — names must stay unique across the merged shader library). ----
struct FluidPresentParams {
    uint width;
    uint height;
    float exposure;
};

// 1. Reset-time clear: velocity, dye and pressure all start at zero. Called
// once (needsInit_), matching the Lenia/RD "init writes the CURRENT read
// side" convention. divergence_ is NOT cleared here — it is fully
// recomputed every frame by fluidDivergence before Jacobi ever reads it.
kernel void fluidClear(texture2d<float, access::write> velOut [[texture(0)]],
                       texture2d<float, access::write> dyeOut [[texture(1)]],
                       texture2d<float, access::write> pressureOut [[texture(2)]],
                       constant FluidParams& p [[buffer(0)]],
                       uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    velOut.write(float4(0.0f, 0.0f, 0.0f, 0.0f), gid);
    dyeOut.write(float4(0.0f, 0.0f, 0.0f, 0.0f), gid);
    pressureOut.write(float4(0.0f, 0.0f, 0.0f, 0.0f), gid);
}

// 2. Semi-Lagrangian velocity advection: trace this pixel's velocity back
// along itself by dt, bilinear-sample the field there, apply dissipation.
// velR is access::sample throughout (both the "velocity at this pixel" read
// used to build the backtrace, and the backtraced bilinear lookup, use the
// same sampler-qualified texture — sampling exactly at a texel center
// returns that texel's value, so the first lookup is exact).
kernel void fluidAdvectVel(texture2d<float, access::sample> velR [[texture(0)]],
                           texture2d<float, access::write> velW [[texture(1)]],
                           constant FluidParams& p [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float2 pos = float2(gid) + 0.5f;
    float w = float(p.width), h = float(p.height);
    float2 v = sampleFieldWrap4(velR, pos, w, h).xy;
    float2 prev = edgePos(pos - v * p.dt, h, p.wallY);
    float2 adv = sampleFieldWrap4(velR, prev, w, h).xy;
    adv *= exp(-p.velDissipation * p.dt);

    velW.write(float4(adv, 0.0f, 0.0f), gid);
}

// 3. Forces: kick impulse (radial, from a hashed point that rehashes
// periodically), hihat turbulence (per-pixel jitter), and fft band forces
// (design doc §1.2 "fft deforms the force field" — low buoys the bottom
// third upward, mid shears the middle band, high stirs the top third).
// All three are added as direct per-pass velocity nudges (no dt factor),
// matching the kick impulse formula given literally in the spec; dissipation
// (fluidAdvectVel) and pressure projection are what keep this bounded.
kernel void fluidForces(texture2d<float, access::read> velR [[texture(0)]],
                        texture2d<float, access::write> velW [[texture(1)]],
                        texture2d<float, access::sample> forceField [[texture(2)]],
                        constant FluidParams& p [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float2 vel = velR.read(gid).xy;
    float2 world = float2(p.width, p.height);
    float2 pixelPos = float2(gid) + 0.5f;

    // Kick impulse: hashed point, radial falloff. NOTE (deviation, see final
    // report): spec prose says "moves every 12 frames" but the given code
    // formula is literally frameIndex/20 — implemented as written (/20).
    // p.seed here is frame-stable (FluidModule::encode() reseeds only by
    // substep, not frameIndex — see the comment there) precisely so this
    // hash stays constant across every frame in the same 20-frame cycle
    // instead of re-randomizing every frame.
    uint cycle = p.frameIndex / 20u;
    float2 impulsePos = float2(rand01(uint2(cycle, 0u), 9001u, p.seed),
                               rand01(uint2(cycle, 1u), 9002u, p.seed)) * world;
    float2 d = pixelPos - impulsePos;
    // Torus branch is the original expression verbatim: splitting it changes
    // FMA contraction under fast-math and breaks bit-exact output.
    if (p.wallY == 0u) {
        d -= world * round(d / world); // minimum image (toroidal)
    } else {
        d.x -= world.x * round(d.x / world.x); // walls: no image across floor/ceiling
    }
    float r2 = dot(d, d);
    float R = max(p.impulseRadius, 1.0f);
    float falloff = exp(-r2 / (R * R));
    float2 dir = (r2 > 1e-6f) ? d * rsqrt(r2) : float2(0.0f, 0.0f);
    vel += p.impulse * falloff * dir;

    // Strip world: a body force along the band. The sin profile is zero at
    // floor/ceiling and peaks mid-height, so it drives shear (eddies rolling
    // along the wall) rather than a rigid scroll of the whole picture.
    if (p.driftX != 0.0f) {
        vel.x += p.driftX * sin(3.14159265359f * pixelPos.y / world.y) * p.dt;
    }

    // hihat -> turbulence: per-pixel random-angle micro force.
    if (p.turbulence > 0.0f) {
        float a = rand01(gid, 5501u + p.frameIndex, p.seed) * 6.28318530718f;
        vel += float2(cos(a), sin(a)) * p.turbulence * 50.0f;
    }

    // fft bands deform the force field itself (design doc §1.2), not just
    // colors. v01: 0 at the top row, 1 at the bottom row.
    float v01 = pixelPos.y / world.y;
    if (v01 > 2.0f / 3.0f) {
        // low -> bottom third: upward buoyancy (toward smaller y).
        vel += float2(0.0f, -p.low * 70.0f);
    } else if (v01 > 1.0f / 3.0f) {
        // mid -> middle band: horizontal shear, sign flips at the midline so
        // it reads as shear rather than uniform drift.
        float side = (pixelPos.x > world.x * 0.5f) ? 1.0f : -1.0f;
        vel += float2(side * p.mid * 55.0f, 0.0f);
    } else {
        // high -> top third: fine turbulent stirring. frameIndex folded into
        // the tag (not p.seed, which FluidModule::encode() deliberately
        // holds frame-stable so the kick impulse position below stays put
        // within a cycle window) so this still animates every frame.
        float a = rand01(gid, 5601u + p.frameIndex, p.seed) * 6.28318530718f;
        vel += float2(cos(a), sin(a)) * p.high * 40.0f;
    }

    // Phase 12: a bound scalar field (e.g. slime trail) stirs the fluid —
    // its gradient pushes velocity from low to high concentration, so trail
    // networks carve currents. Fallback texture is black => zero gradient.
    if (p.forceFieldGain != 0.0f) {
        float e = 2.0f;
        float w = float(p.width), h = float(p.height);
        float gx = sampleFieldWrap(forceField, pixelPos + float2(e, 0.0f), w, h)
                 - sampleFieldWrap(forceField, pixelPos - float2(e, 0.0f), w, h);
        float gy = sampleFieldWrap(forceField, edgePos(pixelPos + float2(0.0f, e), h, p.wallY), w, h)
                 - sampleFieldWrap(forceField, edgePos(pixelPos - float2(0.0f, e), h, p.wallY), w, h);
        if (p.wallY == 0u) {
            vel += float2(gx, gy) * p.forceFieldGain;
        } else {
            // Next to a wall the trail can only pull from one side, so the
            // flow converges onto the wall, carries the agents there, and the
            // whole population collapses onto floor/ceiling. Fade the coupling
            // out over the outer 15% of the height.
            float dWall = min(pixelPos.y, world.y - pixelPos.y);
            vel += float2(gx, gy) * p.forceFieldGain * smoothstep(0.0f, 0.15f * world.y, dWall);
        }
    }

    velW.write(float4(vel, 0.0f, 0.0f), gid);
}

// Curl (2D pseudo-scalar) at an arbitrary integer texel, central difference,
// toroidal. Private to this file — recomputed at 5 neighboring texels inside
// fluidVorticity rather than stored in its own texture (spec explicitly
// allows this: "近傍 curl を再計算するコストは許容、テクスチャを増やさない").
static float fluidCurlAt(texture2d<float, access::read> velR, int2 p, uint w, uint h) {
    float vyxp = velR.read(wrapCoord(p + int2(1, 0), w, h)).y;
    float vyxm = velR.read(wrapCoord(p + int2(-1, 0), w, h)).y;
    float vxyp = velR.read(wrapCoord(p + int2(0, 1), w, h)).x;
    float vxym = velR.read(wrapCoord(p + int2(0, -1), w, h)).x;
    return 0.5f * ((vyxp - vyxm) - (vxyp - vxym));
}

// Walled-strip sibling (y clamps at floor/ceiling). Deliberately a separate
// function: threading a wallY argument through fluidCurlAt changes how
// fast-math reassociates the torus path and breaks bit-exact output of every
// existing fluid scene (verified by bisection against frame MD5s).
static float fluidCurlAtWall(texture2d<float, access::read> velR, int2 p, uint w, uint h) {
    float vyxp = velR.read(edgeCoord(p + int2(1, 0), w, h, 1u)).y;
    float vyxm = velR.read(edgeCoord(p + int2(-1, 0), w, h, 1u)).y;
    float vxyp = velR.read(edgeCoord(p + int2(0, 1), w, h, 1u)).x;
    float vxym = velR.read(edgeCoord(p + int2(0, -1), w, h, 1u)).x;
    return 0.5f * ((vyxp - vyxm) - (vxyp - vxym));
}

// 4. Vorticity confinement (Fedkiw/Stam "Visual Simulation of Smoke" 2001;
// GPU Gems ch.38, standard formula): pushes energy back into small
// rotational structures that numerical dissipation and pressure projection
// would otherwise erase within a few seconds — the "壁境界条件が消える...
// vorticity confinement 必須" requirement (design doc §21.1). Without this
// the field goes visually dead; with it, swirls persist and self-sustain.
kernel void fluidVorticity(texture2d<float, access::read> velR [[texture(0)]],
                           texture2d<float, access::write> velW [[texture(1)]],
                           constant FluidParams& p [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 ip = int2(gid);
    float2 vel = velR.read(gid).xy;

    if (p.vorticity > 0.0f) {
        float c, cxp, cxm, cyp, cym;
        if (p.wallY == 0u) {
            c = fluidCurlAt(velR, ip, p.width, p.height);
            cxp = abs(fluidCurlAt(velR, ip + int2(1, 0), p.width, p.height));
            cxm = abs(fluidCurlAt(velR, ip + int2(-1, 0), p.width, p.height));
            cyp = abs(fluidCurlAt(velR, ip + int2(0, 1), p.width, p.height));
            cym = abs(fluidCurlAt(velR, ip + int2(0, -1), p.width, p.height));
        } else {
            c = fluidCurlAtWall(velR, ip, p.width, p.height);
            cxp = abs(fluidCurlAtWall(velR, ip + int2(1, 0), p.width, p.height));
            cxm = abs(fluidCurlAtWall(velR, ip + int2(-1, 0), p.width, p.height));
            cyp = abs(fluidCurlAtWall(velR, ip + int2(0, 1), p.width, p.height));
            cym = abs(fluidCurlAtWall(velR, ip + int2(0, -1), p.width, p.height));
        }

        float2 grad = 0.5f * float2(cxp - cxm, cyp - cym);
        float len = length(grad);
        float2 n = (len > 1e-5f) ? grad / len : float2(0.0f, 0.0f);
        // force = epsilon * (N x curl_z), curl_z = (0,0,c):
        //   cross(N, (0,0,c)) = (N.y*c, -N.x*c, 0)
        float2 force = p.vorticity * float2(n.y * c, -n.x * c);
        vel += force * p.dt;
    }

    velW.write(float4(vel, 0.0f, 0.0f), gid);
}

// 5. Divergence, central difference, toroidal. Single (non-ping-pong)
// target — fully overwritten every frame, so it never needs clearing.
kernel void fluidDivergence(texture2d<float, access::read> velR [[texture(0)]],
                            texture2d<float, access::write> divW [[texture(1)]],
                            constant FluidParams& p [[buffer(0)]],
                            uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 ip = int2(gid);
    float vxp = velR.read(edgeCoord(ip + int2(1, 0), p.width, p.height, p.wallY)).x;
    float vxm = velR.read(edgeCoord(ip + int2(-1, 0), p.width, p.height, p.wallY)).x;
    float vyp = velR.read(edgeCoord(ip + int2(0, 1), p.width, p.height, p.wallY)).y;
    float vym = velR.read(edgeCoord(ip + int2(0, -1), p.width, p.height, p.wallY)).y;
    float div = 0.5f * ((vxp - vxm) + (vyp - vym));

    divW.write(float4(div, 0.0f, 0.0f, 0.0f), gid);
}

// 6. Jacobi pressure relaxation: p' = (pL+pR+pT+pB - div) / 4. C++ side
// ping-pongs this N times (jacobiIterations, default 28) per substep; no
// pre-loop clear here — pressure warm-starts from last frame's converged
// state (fast convergence). Only reset() clears it (fluidClear above).
kernel void fluidJacobi(texture2d<float, access::read> pR [[texture(0)]],
                        texture2d<float, access::read> divR [[texture(1)]],
                        texture2d<float, access::write> pW [[texture(2)]],
                        constant FluidParams& p [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 ip = int2(gid);
    float pl = pR.read(edgeCoord(ip + int2(-1, 0), p.width, p.height, p.wallY)).x;
    float pr = pR.read(edgeCoord(ip + int2(1, 0), p.width, p.height, p.wallY)).x;
    float pt = pR.read(edgeCoord(ip + int2(0, -1), p.width, p.height, p.wallY)).x;
    float pb = pR.read(edgeCoord(ip + int2(0, 1), p.width, p.height, p.wallY)).x;
    float div = divR.read(gid).x;
    float result = (pl + pr + pt + pb - div) * 0.25f;

    pW.write(float4(result, 0.0f, 0.0f, 0.0f), gid);
}

// 7. Project: vel -= grad(p), central difference, toroidal.
kernel void fluidProject(texture2d<float, access::read> velR [[texture(0)]],
                         texture2d<float, access::read> pR [[texture(1)]],
                         texture2d<float, access::write> velW [[texture(2)]],
                         constant FluidParams& p [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 ip = int2(gid);
    float pl = pR.read(edgeCoord(ip + int2(-1, 0), p.width, p.height, p.wallY)).x;
    float pr = pR.read(edgeCoord(ip + int2(1, 0), p.width, p.height, p.wallY)).x;
    float pt = pR.read(edgeCoord(ip + int2(0, -1), p.width, p.height, p.wallY)).x;
    float pb = pR.read(edgeCoord(ip + int2(0, 1), p.width, p.height, p.wallY)).x;
    float2 grad = 0.5f * float2(pr - pl, pb - pt);
    float2 vel = velR.read(gid).xy - grad;
    if (p.wallY != 0u && (gid.y == 0u || gid.y == p.height - 1u)) vel.y = 0.0f; // no flow through floor/ceiling

    velW.write(float4(vel, 0.0f, 0.0f), gid);
}

// 8. Dye advection (bilinear, by the now-divergence-free velocity) + kick
// dye injection at the same hashed point fluidForces used this frame (same
// formula, so color and motion appear at the same place).
kernel void fluidAdvectDye(texture2d<float, access::sample> dyeR [[texture(0)]],
                           texture2d<float, access::read> velR [[texture(1)]],
                           texture2d<float, access::write> dyeW [[texture(2)]],
                           constant FluidParams& p [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float2 pos = float2(gid) + 0.5f;
    float w = float(p.width), h = float(p.height);
    float2 vel = velR.read(gid).xy; // backtrace vector at this exact texel
    float2 prev = edgePos(pos - vel * p.dt, h, p.wallY);
    float4 dye = sampleFieldWrap4(dyeR, prev, w, h);
    dye.rgb *= exp(-p.dyeDissipation * p.dt);

    float2 world = float2(p.width, p.height);
    uint cycle = p.frameIndex / 20u; // same cadence/formula as fluidForces
    float2 impulsePos = float2(rand01(uint2(cycle, 0u), 9001u, p.seed),
                               rand01(uint2(cycle, 1u), 9002u, p.seed)) * world;
    float2 d = pos - impulsePos;
    if (p.wallY == 0u) {
        d -= world * round(d / world);
    } else {
        d.x -= world.x * round(d.x / world.x);
    }
    float r2 = dot(d, d);
    float R = max(p.impulseRadius, 1.0f);
    float falloff = exp(-r2 / (R * R));

    if (p.dyeInject > 0.0f) {
        // injectHue + a band-centroid bias (weighted toward mid/high energy,
        // 0 when only low is present) — "注入色相の基準" + spectral tilt —
        // plus a per-cycle hash rotation (deviation, see final report): with
        // only the base hue, every cycle injects the exact same color, so
        // even though the velocity field genuinely swirls (verified via a
        // debug visualization), the dye painting looks like same-hue
        // brightness blobs and the swirl reads as barely visible. Rotating
        // hue once per cycle — same rand01(cycle,...) family as impulsePos,
        // so it is seed-derived and stays cycle-stable like the position —
        // makes each cycle's dye visually distinct, so where flow carries
        // one cycle's color into another the mixing is actually legible.
        float bandSum = p.low + p.mid + p.high + 1e-4f;
        float bandCentroid = (p.mid * 0.5f + p.high) / bandSum;
        float hue = p.injectHue + bandCentroid * 0.3f + rand01(uint2(cycle, 2u), 9003u, p.seed);
        float3 col = 0.5f + 0.5f * cos(6.28318530718f *
                     (hue + float3(0.0f, 0.33f, 0.67f)));
        // Dye injection is deliberately decoupled from the velocity impulse
        // scale (impulse lives in px/s units, hundreds under kick; a shared
        // scale flood-fills the impulse radius). dyeInject is a per-frame
        // replacement fraction at the center — silence leaves faint wisps,
        // kick mapping blooms it (review tuning).
        float amount = clamp(falloff * p.dyeInject, 0.0f, 0.5f);
        dye.rgb = mix(dye.rgb, col, amount);
    }

    dyeW.write(float4(dye.rgb, 1.0f), gid);
}

// 9. Present: dye.rgb -> output, exposure only (no ColorMapPass — dye is
// already the picture). alpha = 1.
kernel void fluidPresent(texture2d<float, access::read> dyeR [[texture(0)]],
                         texture2d<float, access::write> out [[texture(1)]],
                         constant FluidPresentParams& p [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    float3 c = dyeR.read(gid).rgb * p.exposure;
    out.write(float4(c, 1.0f), gid);
}
