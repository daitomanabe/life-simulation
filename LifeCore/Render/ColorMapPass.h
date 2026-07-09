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
};

} // namespace life
