// Shaders/Field/PresenceField.metal
// /presence OSC -> 2D field (sensor-agnostic visitor tracking along the
// wall). Each currently-held point rasterizes as a soft radial Gaussian
// blob in pixel space: v += strength * exp(-(r/radiusPx)^2). Distance uses
// the MINIMUM IMAGE in x (the 93m strip wraps, same "d -= w*round(d/w)"
// idiom as Fluid.metal's kick impulse) and plain distance in y (floor/
// ceiling are walls — no wrap, matching CommonTypes.metal's edgeCoord/
// edgePos convention elsewhere in this suite). Asymmetric attack/release
// one-pole smoothing toward that per-frame target is what also implements
// the OSC-loss watchdog fade: with pointCount 0 (nobody held, or the CPU
// side's watchdog has tripped) the target is zero field-wide and release-
// rate smoothing fades to it exactly like FeatureSmoother's idle decay.

struct PresenceFieldParams {
    uint width;
    uint height;
    uint pointCount;
    float radiusPx;
    float attack;
    float release;
    float dt;
    float strengthScale; // already applied per-point on the CPU side; unused here, kept for parity
};

// One float4 per tracked point: (xPx, yPx, strength, 0 padding). Fixed
// capacity mirrors life::kMaxPresencePoints (64) so this buffer's size never
// changes at runtime.
struct PresenceFieldPoints {
    float4 data[64];
};

kernel void presenceFieldClear(texture2d<float, access::write> dst [[texture(0)]],
                               constant PresenceFieldParams& p [[buffer(0)]],
                               uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    dst.write(float4(0, 0, 0, 0), gid);
}

kernel void presenceFieldUpdate(texture2d<float, access::read> src [[texture(0)]],
                                texture2d<float, access::write> dst [[texture(1)]],
                                constant PresenceFieldParams& p [[buffer(0)]],
                                constant PresenceFieldPoints& pts [[buffer(1)]],
                                uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float2 pixelPos = float2(gid) + 0.5f;
    float w = float(p.width);
    float invR2 = 1.0f / max(p.radiusPx * p.radiusPx, 1e-6f);

    float target = 0.0f;
    for (uint i = 0; i < p.pointCount; ++i) {
        float2 c = pts.data[i].xy;
        float strength = pts.data[i].z;
        float dx = pixelPos.x - c.x;
        dx -= w * round(dx / w); // minimum image in x (strip wraps)
        float dy = pixelPos.y - c.y; // plain distance in y (floor/ceiling are walls)
        float r2 = dx * dx + dy * dy;
        target += strength * exp(-r2 * invR2);
    }

    float prev = src.read(gid).x;
    float rate = target > prev ? p.attack : p.release;
    float k = 1.0f - exp(-rate * p.dt);
    float v = prev + (target - prev) * k;

    dst.write(float4(v, 0, 0, 0), gid);
}
