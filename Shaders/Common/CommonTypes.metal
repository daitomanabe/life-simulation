// Shaders/Common/CommonTypes.metal
// Shared types for all LifeCore shaders. Sources under Shaders/ are
// concatenated at runtime (Common/ first) and compiled as ONE library, so
// shared structs are defined exactly once, here.
// Layout must match LifeCore/Sim/SharedTypes.h (scalar-packed).

#include <metal_stdlib>
using namespace metal;

struct AudioUniforms {
    float kick;
    float snare;
    float hihat;
    float perc;
    float beat;

    float rms;
    float low;
    float mid;
    float high;
    float centroid;
    float flux;

    float time;
    float deltaTime;

    float fft[128];

    uint frameIndex;
};

// Boundary modes (matches life::BoundaryMode).
constant uint kBoundaryWrap = 0;
constant uint kBoundaryClamp = 1;
constant uint kBoundaryMirror = 2;
constant uint kBoundaryZero = 3;

// Toroidal neighbor coordinate (Wrap is the VJ default, design doc §7.5).
inline uint2 wrapCoord(int2 p, uint w, uint h) {
    int x = p.x % int(w);
    int y = p.y % int(h);
    if (x < 0) x += int(w);
    if (y < 0) y += int(h);
    return uint2(x, y);
}

// Strip world (a wall-mounted band, e.g. Room B): x stays toroidal, but with
// wallY != 0 the floor/ceiling are walls, so y clamps to the edge row instead
// of wrapping (Neumann for pressure/trail reads). wallY == 0 is exactly
// wrapCoord, so existing scenes are untouched.
inline uint2 edgeCoord(int2 p, uint w, uint h, uint wallY) {
    if (wallY == 0u) return wrapCoord(p, w, h);
    int x = p.x % int(w);
    if (x < 0) x += int(w);
    return uint2(x, clamp(p.y, 0, int(h) - 1));
}

// Pixel-space sample position for the repeat-address samplers in
// Sampling.metal: inside [0.5, h-0.5] a bilinear read never blends across
// the y seam, so clamping here is all a walled strip needs.
inline float2 edgePos(float2 posPx, float h, uint wallY) {
    if (wallY != 0u) posPx.y = clamp(posPx.y, 0.5f, h - 0.5f);
    return posPx;
}
