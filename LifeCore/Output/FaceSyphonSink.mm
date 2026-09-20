// LifeCore/Output/FaceSyphonSink.mm
// Metal/Syphon live here, per the phase 6 spec's carve-out: Output-layer .mm
// files may use Metal directly (MetalInternal.h). Template: SyphonSink.mm.
#include "LifeCore/Output/FaceSyphonSink.h"
#include "LifeCore/Metal/MetalInternal.h"

#import <Syphon/SyphonMetalServer.h>
#import <IOSurface/IOSurface.h>
#import <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <vector>

// Mirrors FacePresentParams in Shaders/Render/FacePresent.metal.
namespace {
struct FacePresentParams {
    uint32_t dstWidth;
    uint32_t dstHeight;
    float uOffset;
    float uScale;
    float exposure;
    float grain;
    uint32_t frame;
};
static_assert(sizeof(FacePresentParams) == 7 * 4,
             "FacePresentParams must stay scalar-packed to match MSL");

// Empty base -> bare "west"/"north"/"east" (or "1".."N"); non-empty base ->
// "<base> west" etc, so --syphon-faces works standalone or alongside
// --syphon without a separate base-name flag.
std::string faceServerName(const std::string& base, uint32_t k, uint32_t n) {
    static const char* kFaceNames[3] = {"west", "north", "east"};
    std::string suffix = n == 3 ? kFaceNames[k] : std::to_string(k + 1);
    return base.empty() ? suffix : base + " " + suffix;
}
} // namespace

namespace life {

struct FaceSyphonSink::Impl {
    struct Face {
        SyphonMetalServer* server = nil;
        TextureHandle texture;
    };
    std::vector<Face> faces;
    ResourcePool* pool = nullptr;
    bool stopped = false;
};

FaceSyphonSink::FaceSyphonSink(std::string baseName, uint32_t faceCount, uint32_t outWidth,
                              uint32_t outHeight, float grainAmount)
    : impl_(std::make_unique<Impl>()),
      baseName_(std::move(baseName)),
      faceCount_(faceCount > 0 ? faceCount : 3),
      outWidth_(outWidth),
      outHeight_(outHeight),
      grainAmount_(grainAmount) {}

FaceSyphonSink::~FaceSyphonSink() { stop(); }

bool FaceSyphonSink::start(MetalContext& metal, ResourcePool& pool, uint32_t width,
                           uint32_t /*height*/, std::string& outError) {
    if (width % faceCount_ != 0) {
        outError = "FaceSyphonSink: width " + std::to_string(width) +
                   " is not evenly divisible by faceCount " + std::to_string(faceCount_);
        return false;
    }
    impl_->pool = &pool;
    impl_->faces.resize(faceCount_);

    @autoreleasepool {
        for (uint32_t k = 0; k < faceCount_; ++k) {
            TextureDesc td;
            td.width = outWidth_;
            td.height = outHeight_;
            td.format = PixelFormat::BGRA8Unorm;
            td.storage = StorageMode::GPUPrivate;
            td.label = baseName_ + " face" + std::to_string(k);
            TextureHandle h = pool.createTexture(td);
            if (!h.valid()) {
                outError = "FaceSyphonSink: failed to allocate face " + std::to_string(k) +
                           " texture (" + std::to_string(outWidth_) + "x" +
                           std::to_string(outHeight_) + ")";
                stop();
                return false;
            }
            impl_->faces[k].texture = h;

            std::string serverName = faceServerName(baseName_, k, faceCount_);
            NSString* name = [NSString stringWithUTF8String:serverName.c_str()];
            SyphonMetalServer* server = [[SyphonMetalServer alloc] initWithName:name
                                                                          device:metal.impl().device
                                                                         options:nil];
            if (!server) {
                outError = "SyphonMetalServer initWithName:device:options: returned nil "
                           "(server name '" +
                           serverName + "')";
                stop();
                return false;
            }
            impl_->faces[k].server = server;
        }
    }
    return true;
}

void FaceSyphonSink::publish(CommandGraph& graph, TextureHandle frame) {
    if (impl_->faces.empty()) return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = graph.impl().commandBuffer;
        if (!cb) return;

        // Grain reseed: the engine's own frame counter (CommandGraph::beginFrame
        // sets this every frame from SceneRunner::frameIndex_ — see
        // MetalInternal.h / SceneRunner::step), NOT a wall clock, so grain is
        // reproducible for a given frame index and identical between realtime
        // and offline playback of the same scene. publish() can see it (already
        // reads graph.impl().commandBuffer below), so there's no need for a
        // private counter on this sink. XORed per face with the same
        // frame-into-seed idiom SceneRunner::step uses for reseeding
        // (seed ^ (frameIndex * 2654435761u)) so the three walls don't show the
        // identical noise pattern.
        const uint32_t frameSeed = graph.impl().frameIndex;
        const float uScale = 1.0f / float(faceCount_);
        for (uint32_t k = 0; k < faceCount_; ++k) {
            uint32_t faceSeed = frameSeed ^ (k * 2654435761u);
            FacePresentParams params{outWidth_,      outHeight_, float(k) * uScale,
                                     uScale,         1.0f,       grainAmount_,
                                     faceSeed};
            graph.pass("facePresent." + std::to_string(k))
                .pipeline("facePresentToBGRA")
                .read(0, frame)
                .write(1, impl_->faces[k].texture)
                .uniforms(0, params)
                .dispatch2D(outWidth_, outHeight_);
        }

        for (uint32_t k = 0; k < faceCount_; ++k) {
            if (!impl_->faces[k].server) continue;
            id<MTLTexture> tex = graph.resources().impl().texture(impl_->faces[k].texture);
            if (!tex) continue;
            NSRect region = NSMakeRect(0, 0, tex.width, tex.height);
            [impl_->faces[k].server publishFrameTexture:tex
                                        onCommandBuffer:cb
                                            imageRegion:region
                                                flipped:NO];
        }
    }
}

void FaceSyphonSink::pump(bool& /*shouldQuit*/) {
    // Same requirement as SyphonSink::pump — server discovery/announce goes
    // over NSDistributedNotificationCenter, which only delivers while this
    // thread services its run loop.
    while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource) {
    }
}

void FaceSyphonSink::stop() {
    if (impl_->stopped) return;
    impl_->stopped = true;
    @autoreleasepool {
        for (auto& f : impl_->faces) {
            if (f.server) {
                [f.server stop];
                f.server = nil;
            }
            if (impl_->pool && f.texture.valid()) impl_->pool->release(f.texture);
        }
        impl_->faces.clear();
    }
}

} // namespace life
