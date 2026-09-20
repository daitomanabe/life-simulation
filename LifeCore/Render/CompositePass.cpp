// LifeCore/Render/CompositePass.cpp
#include "LifeCore/Render/CompositePass.h"

namespace life {

BlendMode blendModeFromString(const std::string& s) {
    if (s == "screen") return BlendMode::Screen;
    if (s == "multiply") return BlendMode::Multiply;
    if (s == "max") return BlendMode::Max;
    if (s == "alpha") return BlendMode::Alpha;
    return BlendMode::Add;
}

const char* blendModeToString(BlendMode m) {
    switch (m) {
        case BlendMode::Screen: return "screen";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Max: return "max";
        case BlendMode::Alpha: return "alpha";
        default: return "add";
    }
}

namespace {
struct CompositeParams {
    uint32_t mode;
    float opacity;
};
struct ClearParams {
    float r, g, b, a;
};
} // namespace

void CompositePass::encode(CommandGraph& graph, const std::vector<CompositeLayer>& layers,
                           TextureHandle target, uint32_t width, uint32_t height) {
    ClearParams clear{0, 0, 0, 1};
    graph.pass("composite.clear")
        .pipeline("clearTexture")
        .write(0, target)
        .uniforms(0, clear)
        .dispatch2D(width, height);

    int i = 0;
    for (const auto& layer : layers) {
        if (!layer.texture.valid()) continue;
        CompositeParams p{uint32_t(layer.mode), layer.opacity};
        graph.pass("composite.layer" + std::to_string(i++))
            .pipeline("compositeLayer")
            .read(0, layer.texture)
            .write(1, target)
            .uniforms(0, p)
            .dispatch2D(width, height);
    }
}

} // namespace life
