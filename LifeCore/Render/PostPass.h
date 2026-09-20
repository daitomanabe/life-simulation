#pragma once
// LifeCore/Render/PostPass.h
// Scene-level post-processing on the final render target, configured by a
// top-level "post" block in the scene JSON:
//   "post": { "bloom": { "threshold": 0.6, "knee": 0.4, "radius": 24, "intensity": 0.5 },
//             "accumulate": { "seconds": 0.0 } }
// radius is in full-resolution px. Without the block nothing is allocated and
// nothing is encoded, so existing scenes are untouched.

#include "LifeCore/Field/PingPongTexture.h"
#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/ResourcePool.h"

#include <nlohmann/json.hpp>

namespace life {

class PostPass {
public:
    // Returns false (with outError) only if bloom or accumulate was
    // requested and its textures could not be allocated.
    bool configure(const nlohmann::json& sceneDoc, ResourcePool& resources, uint32_t width,
                   uint32_t height, std::string& outError);
    // dt: the engine's fixed simulation step (never a wall clock), used only
    // by the accumulate stage below.
    void encode(CommandGraph& graph, TextureHandle target, float dt);

    // dump-state: the bloom block "as used" (radiusPx is the pre-clamp scene
    // value; sigma is the derived quarter-res value the shader actually
    // runs with — clamp(radiusPx/4, 0.5, 16)).
    struct BloomInfo {
        bool enabled = false;
        float threshold = 0.0f, knee = 0.0f, radiusPx = 0.0f, intensity = 0.0f, sigma = 0.0f;
    };
    BloomInfo bloomInfo() const;

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
    float bloomRadiusPx_ = 24.0f; // pre-clamp scene "radius", kept for dump-state
    TextureHandle quarterA_, quarterB_;

    // Mirrors AccumulateParams in Shaders/Render/Post.metal.
    struct AccumulateParams {
        uint32_t width = 0, height = 0;
        float alpha = 0.0f;
    };
    static_assert(sizeof(AccumulateParams) == 3 * 4, "AccumulateParams must stay scalar-packed to match MSL");

    // "post": { "accumulate": { "seconds": <seconds> } }, default 0 = off.
    // Runs after bloom, at the very end of the post chain: exponentially
    // low-pass-filters the fully composited (and bloomed) frame over time
    // into a persistent texture, then writes that filtered result back into
    // `target` so every downstream sink sees it. 0 is a true no-op: nothing
    // allocated, nothing dispatched, byte-identical output.
    float accumulateSeconds_ = 0.0f;
    uint32_t accumWidth_ = 0, accumHeight_ = 0;
    bool accumNeedsSeed_ = true;
    PingPongTexture accum_;
};

} // namespace life
