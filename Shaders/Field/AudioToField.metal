// Shaders/Field/AudioToField.metal
// fft[128] → 2D field. Two modes:
//   0 scroll: spectrogram history — bins across X, first row is the live
//             spectrum, previous rows shift down (time flows downward).
//   1 radial: circular analyzer — bins around the angle, magnitude sets the
//             lit radius; previous field decays exponentially (trail).

struct AudioFieldParams {
    uint mode;
    float gain;
    float decay;
    uint seed;
    uint width;
    uint height;
};

kernel void audioFieldClear(texture2d<float, access::write> dst [[texture(0)]],
                            constant AudioFieldParams& p [[buffer(0)]],
                            uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;
    dst.write(float4(0, 0, 0, 0), gid);
}

kernel void audioFieldUpdate(texture2d<float, access::read> src [[texture(0)]],
                             texture2d<float, access::write> dst [[texture(1)]],
                             constant AudioFieldParams& p [[buffer(0)]],
                             constant AudioUniforms& audio [[buffer(1)]],
                             uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.width || gid.y >= p.height) return;

    float v;
    if (p.mode == 1u) {
        // Radial analyzer with decay trail.
        float2 uv = (float2(gid) + 0.5f) / float2(p.width, p.height);
        float2 d = uv - 0.5f;
        d.x *= float(p.width) / float(p.height);
        float r = length(d) * 2.2f;             // 0 center → ~1.1 edge
        float theta = atan2(d.y, d.x) * 0.15915494309f + 0.5f; // [0,1)
        uint bin = min(uint(theta * 128.0f), 127u);
        float mag = clamp(audio.fft[bin] * p.gain, 0.0f, 1.0f);
        float bar = smoothstep(mag + 0.02f, mag - 0.02f, r); // 1 inside bar
        float prev = src.read(gid).x * p.decay;
        v = max(prev, bar * mag);
    } else {
        // Scrolling spectrogram: row 0 = live spectrum, rows shift down.
        if (gid.y == 0u) {
            float fx = float(gid.x) / float(p.width) * 128.0f;
            uint bin = min(uint(fx), 127u);
            v = clamp(audio.fft[bin] * p.gain, 0.0f, 1.0f);
        } else {
            v = src.read(uint2(gid.x, gid.y - 1u)).x;
        }
    }

    dst.write(float4(v, 0, 0, 0), gid);
}
