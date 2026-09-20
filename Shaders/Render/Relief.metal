// Shaders/Render/Relief.metal
// Field scalar → lit RGBA16F layer: the field is read as a HEIGHT FIELD and
// shaded like a sculpted relief on black — directional diffuse + specular, a
// fresnel-style rim (glassy membranes), iso-height contour lines (fine strut /
// layered-line detail) and a short height-field shadow march toward the light.
// One same-size pass, ~5 reads per empty pixel and 5 + shadowSteps per covered
// one. An alternative to colorMapField, chosen by a module's "relief" params
// block (ColorMapPass::configure). Matches life::ReliefParams (scalar-packed).

struct ReliefParams {
    uint width;
    uint height;
    uint channel;
    uint frame;          // grain reseed
    float inputScale;    // h = 1 - exp(-inputScale * v): compresses HDR fields (slime trail) to 0..1
    float heightScale;   // relief depth in px per unit h (steeper normals, longer shadows)
    float lightX, lightY, lightZ; // unit vector TOWARD the light. x right, y down, z out of the wall
    float ambient;
    float diffuse;
    float specular;
    float shininess;
    float rim;
    float contourFreq;   // iso-lines per unit h (0 = off)
    float contourGain;
    float contourWidth;  // px
    uint shadowSteps;    // 0 = off
    float shadowLength;  // px marched toward the light
    float shadowStrength;
    float tintR, tintG, tintB;
    float grain;
    float exposure;
    float logCurve;      // 0: h = 1 - exp(-k v).  1: h = log(1 + k v) / log(1 + logRange)
    float logRange;      // k*v that maps to h = 1 on the log curve
    float edgeFade;      // px: fade to black toward floor/ceiling (walled strips pile trail there). 0 = off
};

static float reliefHeight(texture2d<float, access::read> field, int2 q,
                          constant ReliefParams& p) {
    // x wraps, y clamps: right for a walled strip, and a one-row shading
    // difference at the top/bottom edge of a torus scene is invisible.
    uint2 c = edgeCoord(q, p.width, p.height, 1u);
    float kv = p.inputScale * max(field.read(c)[min(p.channel, 3u)], 0.0f);
    // A trail decays ~exponentially away from its source, so on the log curve
    // height falls off linearly with distance: slopes stay gentle far out and
    // iso-lines come out evenly spaced around each body instead of piling up
    // on its steep flank.
    if (p.logCurve > 0.5f) return min(log(1.0f + kv) / log(1.0f + p.logRange), 1.0f);
    return 1.0f - exp(-kv);
}

kernel void reliefShadeField(texture2d<float, access::read> field [[texture(0)]],
                             texture2d<float, access::write> dst [[texture(1)]],
                             constant ReliefParams& p [[buffer(0)]],
                             uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    int2 ip = int2(gid);
    float h = reliefHeight(field, ip, p);
    float hxp = reliefHeight(field, ip + int2(1, 0), p);
    float hxm = reliefHeight(field, ip + int2(-1, 0), p);
    float hyp = reliefHeight(field, ip + int2(0, 1), p);
    float hym = reliefHeight(field, ip + int2(0, -1), p);

    // Nothing here and nothing next door: black, skip all the lighting.
    float hMax = max(max(h, hxp), max(max(hxm, hyp), hym));
    if (hMax < 0.004f) {
        dst.write(float4(0.0f, 0.0f, 0.0f, 0.0f), gid);
        return;
    }

    float2 grad = 0.5f * float2(hxp - hxm, hyp - hym); // per px
    float3 n = normalize(float3(-grad * p.heightScale, 1.0f));
    float3 L = float3(p.lightX, p.lightY, p.lightZ);
    float3 H = normalize(L + float3(0.0f, 0.0f, 1.0f)); // viewer looks straight at the wall

    float presence = smoothstep(0.004f, 0.06f, h);
    float ndl = max(dot(n, L), 0.0f);
    float spec = pow(max(dot(n, H), 0.0f), p.shininess);
    float fres = 1.0f - n.z;
    float rim = fres * fres;

    // Height-field shadow: walk toward the light; anything standing above the
    // ray shades this pixel. Soft edge from how far it pokes above the ray.
    float shadow = 1.0f;
    float lxy = length(L.xy);
    if (p.shadowSteps > 0u && lxy > 1e-3f) {
        float2 dir = L.xy / lxy;
        float rise = L.z / lxy; // ray height gained per px travelled
        float base = h * p.heightScale;
        float occ = 0.0f;
        for (uint i = 1u; i <= p.shadowSteps; ++i) {
            float t = p.shadowLength * float(i) / float(p.shadowSteps);
            float hs = reliefHeight(field, ip + int2(round(dir * t)), p) * p.heightScale;
            occ = max(occ, (hs - (base + rise * t)) / (0.15f * p.heightScale));
        }
        shadow = 1.0f - p.shadowStrength * saturate(occ);
    }

    // Iso-height lines, anti-aliased by the local slope (distance to the
    // nearest line in px). Lit like the surface, so they read as struts.
    float lines = 0.0f;
    if (p.contourFreq > 0.0f) {
        float c = h * p.contourFreq;
        float perPx = max(length(grad) * p.contourFreq, 1e-4f); // lines crossed per px
        float distPx = abs(fract(c - 0.5f) - 0.5f) / perPx;
        lines = (1.0f - smoothstep(0.5f * p.contourWidth, 0.5f * p.contourWidth + 1.0f, distPx)) *
                smoothstep(0.02f, 0.10f, h) *
                (1.0f - smoothstep(0.33f, 0.66f, perPx)); // closer than ~2-3 px apart: fade out, not solid white
    }

    float lit = p.ambient + p.diffuse * ndl * shadow;
    float v = presence * lit
            + p.specular * spec * shadow * presence
            + p.rim * rim
            + p.contourGain * lines * (0.35f + 0.65f * ndl * shadow);

    if (p.edgeFade > 0.0f) {
        float dEdge = min(float(gid.y), float(p.height - 1u - gid.y));
        v *= smoothstep(0.0f, p.edgeFade, dEdge);
    }

    if (p.grain > 0.0f) {
        v *= 1.0f + p.grain * (rand01(gid, 7717u + p.frame, 0x52454C31u) - 0.5f);
    }

    float3 rgb = max(v, 0.0f) * float3(p.tintR, p.tintG, p.tintB) * p.exposure;
    dst.write(float4(rgb, presence), gid);
}

// Optional temporal low-pass for the field reliefShadeField reads (design
// doc: SlimeMold trail flicker — agents deposit stochastically, so any pixel
// jitters frame to frame; that jitter reads as sparkle once shaded as a
// height field). ColorMapPass::encode ping-pongs a persistent texture through
// this kernel when "relief.temporalSmoothing" > 0, then hands reliefShadeField
// that smoothed texture instead of the raw field — reliefShadeField above is
// untouched. alpha = 1 - exp(-dt/tau) is computed on the CPU (fixed sim dt,
// never a wall clock). Matches life::ReliefSmoothParams (scalar-packed,
// private to ColorMapPass.cpp).
struct ReliefSmoothParams {
    uint width;
    uint height;
    float alpha;
};

kernel void reliefSmoothField(texture2d<float, access::read> prevSmoothed [[texture(0)]],
                              texture2d<float, access::read> field [[texture(1)]],
                              texture2d<float, access::write> dst [[texture(2)]],
                              constant ReliefSmoothParams& p [[buffer(0)]],
                              uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    float4 prev = prevSmoothed.read(gid);
    float4 cur = field.read(gid);
    dst.write(mix(prev, cur, p.alpha), gid);
}
