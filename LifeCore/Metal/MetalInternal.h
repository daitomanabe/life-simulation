#pragma once
// LifeCore/Metal/MetalInternal.h
// Objective-C++ ONLY. Shared Impl definitions so the .mm translation units can
// reach each other's Metal objects. Never include this from a .cpp file.

#ifndef __OBJC__
#error "MetalInternal.h is Objective-C++ only; include it from .mm files"
#endif

#import <Metal/Metal.h>

#include "LifeCore/Metal/MetalContext.h"
#include "LifeCore/Metal/PipelineCache.h"
#include "LifeCore/Metal/ResourcePool.h"
#include "LifeCore/Metal/GPUTimer.h"
#include "LifeCore/Metal/CommandGraph.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace life {

struct MetalContext::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    std::string shaderRoot;
    bool enableGPUTiming = true;
    bool timingSupported = false;

    // Concatenates all .metal sources under shaderRoot (Common/ first) and
    // compiles a single MTLLibrary.
    bool compileLibrary(std::string& outError);
};

struct PipelineCache::Impl {
    MetalContext* ctx = nullptr;
    std::unordered_map<std::string, id<MTLComputePipelineState>> pipelines;

    // Returns nil and fills outError if the function is missing / fails.
    id<MTLComputePipelineState> pipeline(const std::string& name, std::string& outError);
};

struct ResourcePool::Impl {
    MetalContext* ctx = nullptr;

    struct TextureSlot {
        id<MTLTexture> texture = nil;
        TextureDesc desc;
        uint32_t generation = 1;
        bool alive = false;
    };
    struct BufferSlot {
        id<MTLBuffer> buffer = nil;
        BufferDesc desc;
        uint32_t generation = 1;
        bool alive = false;
    };

    std::vector<TextureSlot> textures;
    std::vector<BufferSlot> buffers;
    size_t textureBytes = 0;
    size_t bufferBytes = 0;

    id<MTLTexture> texture(TextureHandle h) const;
    id<MTLBuffer> buffer(BufferHandle h) const;
};

MTLPixelFormat toMTLPixelFormat(PixelFormat f);

struct GPUTimer::Impl {
    MetalContext* ctx = nullptr;
    bool supported = false;

    id<MTLCounterSet> timestampCounterSet = nil;
    id<MTLCounterSampleBuffer> sampleBuffer = nil;
    static constexpr uint32_t kMaxPasses = 256; // 2 samples per pass

    // Per-frame bookkeeping
    std::vector<std::string> pendingLabels; // pass i -> samples [2i, 2i+1]
    std::vector<PassTiming> lastTimings;
    double lastTotalMs = 0.0;

    // GPU timestamp -> nanoseconds calibration (Apple GPUs report normalized
    // nanosecond timestamps; we calibrate anyway to stay driver-agnostic).
    double gpuTimebaseScale = 1.0; // ns per tick
    void calibrate();

    void beginFrame();
    // Returns the sample index pair base for this pass, or UINT32_MAX if full.
    uint32_t registerPass(const std::string& label);
    void resolve(double wholeBufferMs);
};

struct CommandGraph::Impl {
    MetalContext* ctx = nullptr;
    ResourcePool* pool = nullptr;
    PipelineCache* pipelines = nullptr;

    id<MTLCommandBuffer> commandBuffer = nil;
    uint32_t frameIndex = 0;
    bool open = false;

    GPUTimer timer;
    uint32_t passCountThisFrame = 0;
    double lastWholeBufferMs = 0.0;
    bool lastFrameHadError = false;

    Impl(MetalContext& c, ResourcePool& p, PipelineCache& pl)
        : ctx(&c), pool(&p), pipelines(&pl), timer(c) {}
};

} // namespace life
