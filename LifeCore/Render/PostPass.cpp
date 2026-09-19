// LifeCore/Render/PostPass.cpp
#include "LifeCore/Render/PostPass.h"

#include <algorithm>

namespace life {

bool PostPass::configure(const nlohmann::json& sceneDoc, ResourcePool& resources, uint32_t width,
                         uint32_t height, std::string& outError) {
    if (!sceneDoc.contains("post") || !sceneDoc["post"].contains("bloom")) return true;
    const auto& b = sceneDoc["post"]["bloom"];

    bloom_.srcWidth = width;
    bloom_.srcHeight = height;
    bloom_.dstWidth = (width + 3) / 4;
    bloom_.dstHeight = (height + 3) / 4;
    bloom_.threshold = b.value("threshold", bloom_.threshold);
    bloom_.knee = b.value("knee", bloom_.knee);
    bloom_.intensity = b.value("intensity", bloom_.intensity);
    // radius is given in full-res px; the blur runs at quarter res.
    bloom_.sigma = std::clamp(b.value("radius", 24.0f) / 4.0f, 0.5f, 16.0f);

    TextureDesc td;
    td.width = bloom_.dstWidth;
    td.height = bloom_.dstHeight;
    td.format = PixelFormat::RGBA16F;
    td.label = "post.bloomA";
    quarterA_ = resources.createTexture(td);
    td.label = "post.bloomB";
    quarterB_ = resources.createTexture(td);
    if (!quarterA_.valid() || !quarterB_.valid()) {
        outError = "failed to allocate bloom textures";
        return false;
    }
    bloomEnabled_ = true;
    return true;
}

void PostPass::encode(CommandGraph& graph, TextureHandle target) {
    if (!bloomEnabled_) return;

    graph.pass("post.bloomDown")
        .pipeline("bloomDown")
        .read(0, target)
        .write(1, quarterA_)
        .uniforms(0, bloom_)
        .dispatch2D(bloom_.dstWidth, bloom_.dstHeight);

    BloomParams h = bloom_, v = bloom_;
    h.horizontal = 1;
    v.horizontal = 0;
    graph.pass("post.bloomBlurH")
        .pipeline("bloomBlur")
        .read(0, quarterA_)
        .write(1, quarterB_)
        .uniforms(0, h)
        .dispatch2D(bloom_.dstWidth, bloom_.dstHeight);
    graph.pass("post.bloomBlurV")
        .pipeline("bloomBlur")
        .read(0, quarterB_)
        .write(1, quarterA_)
        .uniforms(0, v)
        .dispatch2D(bloom_.dstWidth, bloom_.dstHeight);

    graph.pass("post.bloomAdd")
        .pipeline("bloomAdd")
        .read(0, quarterA_)
        .write(1, target)
        .uniforms(0, bloom_)
        .dispatch2D(bloom_.srcWidth, bloom_.srcHeight);
}

} // namespace life
