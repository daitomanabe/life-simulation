#pragma once
// LifeCore/Render/ColorMapPass.h
// Converts a scalar/2ch field texture into an RGBA16F visual layer using a
// cosine palette (Inigo Quilez style: color = a + b*cos(2π(c*t + d))).
// Every field module runs one of these to satisfy the "all outputs are
// RGBA16F" rule (design doc §13.1).

#include "LifeCore/Metal/CommandGraph.h"

#include <nlohmann/json.hpp>

namespace life {

// Mirrors ColorMapParams in Shaders/Render/ColorMap.metal.
struct ColorMapParams {
    float aR = 0.5f, aG = 0.5f, aB = 0.5f;
    float bR = 0.5f, bG = 0.5f, bB = 0.5f;
    float cR = 1.0f, cG = 1.0f, cB = 1.0f;
    float dR = 0.00f, dG = 0.10f, dB = 0.20f;
    float inputScale = 1.0f;
    float inputBias = 0.0f;
    float exposure = 1.0f;
    uint32_t channel = 1; // which source channel drives the palette (RD: V)
};

// Mirrors ReliefParams in Shaders/Render/Relief.metal. An alternative to the
// palette: the field is shaded as a lit height field on black.
struct ReliefParams {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channel = 0;
    uint32_t frame = 0;
    float inputScale = 0.25f;
    float heightScale = 24.0f;
    float lightX = -0.55f, lightY = -0.60f, lightZ = 0.58f; // normalized in configure()
    float ambient = 0.06f;
    float diffuse = 0.85f;
    float specular = 0.9f;
    float shininess = 28.0f;
    float rim = 0.35f;
    float contourFreq = 0.0f;
    float contourGain = 0.6f;
    float contourWidth = 1.2f;
    uint32_t shadowSteps = 12;
    float shadowLength = 40.0f;
    float shadowStrength = 0.85f;
    float tintR = 0.93f, tintG = 0.97f, tintB = 1.0f;
    float grain = 0.0f;
    float exposure = 1.0f;
    float logCurve = 0.0f;
    float logRange = 200.0f;
    float edgeFade = 0.0f;
};
static_assert(sizeof(ReliefParams) == 28 * 4, "ReliefParams must stay scalar-packed to match MSL");

class ColorMapPass {
public:
    // Reads palette overrides from a module's JSON params block, e.g.
    // "colorMap": { "a": [...], "b": [...], "c": [...], "d": [...],
    //               "channel": 1, "inputScale": 3.0 }
    void configure(const nlohmann::json& moduleParams);

    void encode(CommandGraph& graph, const std::string& label, TextureHandle field,
                TextureHandle rgbaOut, uint32_t width, uint32_t height);

    // Phase 9 (docs/specs/phase9_lenia_fft.md §1): same palette mapping as
    // encode() above, but field and rgbaOut may differ in size (Lenia's
    // simWidth/simHeight vs the scene's output resolution) — field is read
    // with a normalized-coordinate bilinear sample instead of a same-size
    // texel read. When srcW/srcH == dstW/dstH this samples exactly at texel
    // centers, which is bit-identical to a direct read (spec §1), but
    // encode()/colorMapField are kept as-is for callers that need them
    // (existing modules, and Lenia's own default same-size path — kept on
    // the original codepath deliberately, not switched to always call this,
    // to keep the backward-compat contract's diff to zero for that path).
    void encodeScaled(CommandGraph& graph, const std::string& label, TextureHandle field,
                      TextureHandle rgbaOut, uint32_t srcWidth, uint32_t srcHeight,
                      uint32_t dstWidth, uint32_t dstHeight);

    ColorMapParams params;

    // Set by a "relief": { ... } block in the module's params. When on,
    // encode() shades with reliefShadeField instead of the palette.
    // encodeScaled() (Lenia at a separate sim resolution) still uses the palette.
    bool reliefEnabled = false;
    ReliefParams relief;

private:
    // "lightSpin": degrees per second (at 60 fps) the light turns about the
    // wall normal, so highlights and shadows sweep slowly across the relief.
    float lightSpin_ = 0.0f;
    float baseLightX_ = 0.0f, baseLightY_ = 0.0f;
};

} // namespace life
