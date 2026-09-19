#pragma once
// LifeCore/Render/PostPass.h
// Scene-level post-processing on the final render target, configured by a
// top-level "post" block in the scene JSON:
//   "post": { "bloom": { "threshold": 0.6, "knee": 0.4, "radius": 24, "intensity": 0.5 } }
// radius is in full-resolution px. Without the block nothing is allocated and
// nothing is encoded, so existing scenes are untouched.

#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/ResourcePool.h"

#include <nlohmann/json.hpp>

namespace life {

class PostPass {
public:
    // Returns false (with outError) only if bloom was requested and its
    // quarter-res textures could not be allocated.
    bool configure(const nlohmann::json& sceneDoc, ResourcePool& resources, uint32_t width,
                   uint32_t height, std::string& outError);
    void encode(CommandGraph& graph, TextureHandle target);

private:
    // Mirrors BloomParams in Shaders/Render/Post.metal.
    struct BloomParams {
        uint32_t srcWidth = 0, srcHeight = 0, dstWidth = 0, dstHeight = 0;
        float threshold = 0.6f;
        float knee = 0.4f;
        float sigma = 6.0f;
        float intensity = 0.5f;
        uint32_t horizontal = 1;
    };
    static_assert(sizeof(BloomParams) == 9 * 4, "BloomParams must stay scalar-packed to match MSL");

    bool bloomEnabled_ = false;
    BloomParams bloom_;
    TextureHandle quarterA_, quarterB_;
};

} // namespace life
