// LifeCore/Render/PostPass.cpp
#include "LifeCore/Render/PostPass.h"

#include <algorithm>
#include <cmath>

namespace life {

bool PostPass::configure(const nlohmann::json& sceneDoc, ResourcePool& resources, uint32_t width,
                         uint32_t height, std::string& outError) {
    if (!sceneDoc.contains("post")) return true;
    const auto& post = sceneDoc["post"];

    if (post.contains("bloom")) {
        const auto& b = post["bloom"];

        bloom_.srcWidth = width;
        bloom_.srcHeight = height;
        bloom_.dstWidth = (width + 3) / 4;
        bloom_.dstHeight = (height + 3) / 4;
        bloom_.threshold = b.value("threshold", bloom_.threshold);
        bloom_.knee = b.value("knee", bloom_.knee);
        bloom_.intensity = b.value("intensity", bloom_.intensity);
        // radius is given in full-res px; the blur runs at quarter res.
        bloomRadiusPx_ = b.value("radius", 24.0f);
        bloom_.sigma = std::clamp(bloomRadiusPx_ / 4.0f, 0.5f, 16.0f);

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
    }

    if (post.contains("accumulate")) {
        accumulateSeconds_ = post["accumulate"].value("seconds", 0.0f);
        if (accumulateSeconds_ > 0.0f) {
            accumWidth_ = width;
            accumHeight_ = height;
            TextureDesc td;
            td.width = width;
            td.height = height;
            td.format = PixelFormat::RGBA16F;
            td.label = "post.accumulate";
            if (!accum_.create(resources, td)) {
                outError = "failed to allocate accumulate texture";
                return false;
            }
            accumNeedsSeed_ = true;
        }
    }

    return true;
}

void PostPass::encode(CommandGraph& graph, TextureHandle target, float dt) {
    if (bloomEnabled_) {
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

    // Output-stage temporal accumulation, last in the chain (after bloom) so
    // it smooths exactly what every sink is about to see. Same ping-pong +
    // seed-from-first-frame idiom as ColorMapPass's relief.temporalSmoothing.
    if (accumulateSeconds_ > 0.0f) {
        if (accumNeedsSeed_) {
            // Seed from the composited frame itself, not black, so frame 0
            // is unchanged (acc == frame => mix is a no-op).
            graph.copyTexture(target, accum_.write());
            accumNeedsSeed_ = false;
        } else {
            float alpha = 1.0f - std::exp(-dt / accumulateSeconds_);
            AccumulateParams p{accumWidth_, accumHeight_, alpha};
            graph.pass("post.accumulate")
                .pipeline("postAccumulate")
                .read(0, accum_.read())
                .write(1, target)
                .write(2, accum_.write())
                .uniforms(0, p)
                .dispatch2D(accumWidth_, accumHeight_);
        }
        accum_.swap();
    }
}

PostPass::BloomInfo PostPass::bloomInfo() const {
    BloomInfo info;
    info.enabled = bloomEnabled_;
    if (!bloomEnabled_) return info;
    info.threshold = bloom_.threshold;
    info.knee = bloom_.knee;
    info.radiusPx = bloomRadiusPx_;
    info.intensity = bloom_.intensity;
    info.sigma = bloom_.sigma;
    return info;
}

} // namespace life
