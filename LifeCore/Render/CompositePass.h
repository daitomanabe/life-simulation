#pragma once
// LifeCore/Render/CompositePass.h
// Blends module output layers into the final RGBA16F render target
// (design doc §13.2). Modes: add / screen / multiply / max / alpha.

#include "LifeCore/Metal/CommandGraph.h"

#include <string>
#include <vector>

namespace life {

enum class BlendMode : uint32_t {
    Add = 0,
    Screen = 1,
    Multiply = 2,
    Max = 3,
    Alpha = 4,
};

BlendMode blendModeFromString(const std::string& s);
const char* blendModeToString(BlendMode m); // for dump-state JSON

struct CompositeLayer {
    TextureHandle texture;
    BlendMode mode = BlendMode::Add;
    float opacity = 1.0f;
};

class CompositePass {
public:
    // Clears target, then folds each layer in order.
    void encode(CommandGraph& graph, const std::vector<CompositeLayer>& layers,
                TextureHandle target, uint32_t width, uint32_t height);
};

} // namespace life
