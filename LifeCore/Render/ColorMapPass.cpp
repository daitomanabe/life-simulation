// LifeCore/Render/ColorMapPass.cpp
#include "LifeCore/Render/ColorMapPass.h"

namespace life {

namespace {
// Mirrors ColorMapScaledParams in Shaders/Render/ColorMap.metal. Only the
// destination size is needed on the GPU side — the sampler already knows
// the source texture's own dimensions (§1).
struct ColorMapScaledParams {
    uint32_t dstWidth;
    uint32_t dstHeight;
};
} // namespace

void ColorMapPass::configure(const nlohmann::json& moduleParams) {
    if (!moduleParams.contains("colorMap")) return;
    const auto& cm = moduleParams["colorMap"];

    auto vec3 = [&](const char* key, float& r, float& g, float& b) {
        if (cm.contains(key) && cm[key].is_array() && cm[key].size() >= 3) {
            r = cm[key][0].get<float>();
            g = cm[key][1].get<float>();
            b = cm[key][2].get<float>();
        }
    };
    vec3("a", params.aR, params.aG, params.aB);
    vec3("b", params.bR, params.bG, params.bB);
    vec3("c", params.cR, params.cG, params.cB);
    vec3("d", params.dR, params.dG, params.dB);
    params.inputScale = cm.value("inputScale", params.inputScale);
    params.inputBias = cm.value("inputBias", params.inputBias);
    params.exposure = cm.value("exposure", params.exposure);
    params.channel = cm.value("channel", params.channel);
}

void ColorMapPass::encode(CommandGraph& graph, const std::string& label,
                          TextureHandle field, TextureHandle rgbaOut, uint32_t width,
                          uint32_t height) {
    graph.pass(label)
        .pipeline("colorMapField")
        .read(0, field)
        .write(1, rgbaOut)
        .uniforms(0, params)
        .dispatch2D(width, height);
}

void ColorMapPass::encodeScaled(CommandGraph& graph, const std::string& label,
                                TextureHandle field, TextureHandle rgbaOut, uint32_t srcWidth,
                                uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight) {
    (void)srcWidth;  // unused on the GPU side (see ColorMapScaledParams comment above);
    (void)srcHeight; // kept as parameters for caller clarity / future use.
    ColorMapScaledParams sp{dstWidth, dstHeight};
    graph.pass(label)
        .pipeline("colorMapFieldScaled")
        .read(0, field)
        .write(1, rgbaOut)
        .uniforms(0, params)
        .uniforms(1, sp)
        .dispatch2D(dstWidth, dstHeight);
}

} // namespace life
