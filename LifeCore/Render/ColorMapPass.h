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

    ColorMapParams params;
};

} // namespace life
