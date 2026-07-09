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
