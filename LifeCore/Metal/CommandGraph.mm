// LifeCore/Metal/CommandGraph.mm
#include "LifeCore/Metal/MetalInternal.h"

namespace life {

// ---- ComputePass ----

ComputePass::ComputePass(CommandGraph& graph, std::string label)
    : graph_(graph), label_(std::move(label)) {}

ComputePass& ComputePass::pipeline(std::string functionName) {
    function_ = std::move(functionName);
    return *this;
}

ComputePass& ComputePass::read(int index, TextureHandle h) {
    textures_.push_back({index, h});
    return *this;
}

ComputePass& ComputePass::write(int index, TextureHandle h) {
    textures_.push_back({index, h});
    return *this;
}

ComputePass& ComputePass::buffer(int index, BufferHandle h, size_t offset) {
    buffers_.push_back({index, h, offset});
    return *this;
}

ComputePass& ComputePass::bytes(int index, const void* data, size_t size) {
    ByteBind b;
    b.index = index;
    b.data.assign(static_cast<const uint8_t*>(data),
                  static_cast<const uint8_t*>(data) + size);
    bytes_.push_back(std::move(b));
    return *this;
}

void ComputePass::dispatch2D(uint32_t width, uint32_t height) {
    graph_.dispatch(*this, width, height, 1);
}

void ComputePass::dispatch1D(uint32_t count) { graph_.dispatch(*this, count, 1, 1); }

// ---- CommandGraph ----

CommandGraph::CommandGraph(MetalContext& ctx, ResourcePool& pool, PipelineCache& pipelines)
    : impl_(std::make_unique<Impl>(ctx, pool, pipelines)) {}

CommandGraph::~CommandGraph() = default;

void CommandGraph::beginFrame(uint32_t frameIndex) {
    @autoreleasepool {
        if (impl_->open) {
            fprintf(stderr, "[life] CommandGraph::beginFrame called while frame open\n");
            endFrame(true);
        }
        impl_->commandBuffer = [impl_->ctx->impl().queue commandBuffer];
        impl_->commandBuffer.label =
            [NSString stringWithFormat:@"LifeFrame %u", frameIndex];
        impl_->frameIndex = frameIndex;
        impl_->passCountThisFrame = 0;
        impl_->open = true;
        impl_->timer.impl().beginFrame();
    }
}

void CommandGraph::endFrame(bool waitUntilCompleted) {
    if (!impl_->open) return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = impl_->commandBuffer;
        [cb commit];
        if (waitUntilCompleted) {
            [cb waitUntilCompleted];
            if (cb.error) {
                fprintf(stderr, "[life] command buffer error: %s\n",
                        [[cb.error localizedDescription] UTF8String]);
            }
            double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
            impl_->lastWholeBufferMs = ms;
            impl_->timer.impl().resolve(ms);
        }
        impl_->commandBuffer = nil;
        impl_->open = false;
    }
}

bool CommandGraph::frameOpen() const { return impl_->open; }

void CommandGraph::dispatch(ComputePass& pass, uint32_t gridW, uint32_t gridH,
                            uint32_t gridD) {
    if (!impl_->open) {
        fprintf(stderr, "[life] pass '%s' dispatched outside beginFrame/endFrame\n",
                pass.label_.c_str());
        return;
    }
    @autoreleasepool {
        std::string err;
        id<MTLComputePipelineState> pso =
            impl_->pipelines->impl().pipeline(pass.function_, err);
        if (!pso) {
            fprintf(stderr, "[life] pass '%s': %s\n", pass.label_.c_str(), err.c_str());
            return;
        }

        // Attach counter samples at encoder boundaries for per-pass timing.
        id<MTLComputeCommandEncoder> enc = nil;
        uint32_t sampleBase = impl_->timer.impl().registerPass(pass.label_);
        if (sampleBase != UINT32_MAX) {
            MTLComputePassDescriptor* pd = [MTLComputePassDescriptor computePassDescriptor];
            MTLComputePassSampleBufferAttachmentDescriptor* att =
                pd.sampleBufferAttachments[0];
            att.sampleBuffer = impl_->timer.impl().sampleBuffer;
            att.startOfEncoderSampleIndex = sampleBase;
            att.endOfEncoderSampleIndex = sampleBase + 1;
            enc = [impl_->commandBuffer computeCommandEncoderWithDescriptor:pd];
        } else {
            enc = [impl_->commandBuffer computeCommandEncoder];
        }
        enc.label = [NSString stringWithUTF8String:pass.label_.c_str()];
        [enc setComputePipelineState:pso];

        for (auto& t : pass.textures_) {
            id<MTLTexture> tex = impl_->pool->impl().texture(t.handle);
            if (!tex) {
                fprintf(stderr, "[life] pass '%s': invalid texture at index %d\n",
                        pass.label_.c_str(), t.index);
            }
            [enc setTexture:tex atIndex:t.index];
        }
        for (auto& b : pass.buffers_) {
            id<MTLBuffer> buf = impl_->pool->impl().buffer(b.handle);
            if (!buf) {
                fprintf(stderr, "[life] pass '%s': invalid buffer at index %d\n",
                        pass.label_.c_str(), b.index);
            }
            [enc setBuffer:buf offset:b.offset atIndex:b.index];
        }
        for (auto& b : pass.bytes_) {
            [enc setBytes:b.data.data() length:b.data.size() atIndex:b.index];
        }

        // Non-uniform threadgroups (Apple GPU family 4+; fine on Apple Silicon).
        NSUInteger w = pso.threadExecutionWidth;
        NSUInteger h = std::max<NSUInteger>(1, pso.maxTotalThreadsPerThreadgroup / w);
        if (gridH == 1 && gridD == 1) {
            [enc dispatchThreads:MTLSizeMake(gridW, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(gridW, w * h), 1, 1)];
        } else {
            [enc dispatchThreads:MTLSizeMake(gridW, gridH, gridD)
                threadsPerThreadgroup:MTLSizeMake(w, std::min<NSUInteger>(h, 16), 1)];
        }
        [enc endEncoding];
        impl_->passCountThisFrame++;
    }
}

void CommandGraph::copyTexture(TextureHandle src, TextureHandle dst) {
    if (!impl_->open) return;
    @autoreleasepool {
        id<MTLTexture> s = impl_->pool->impl().texture(src);
        id<MTLTexture> d = impl_->pool->impl().texture(dst);
        if (!s || !d) return;
        id<MTLBlitCommandEncoder> blit = [impl_->commandBuffer blitCommandEncoder];
        blit.label = @"copyTexture";
        [blit copyFromTexture:s toTexture:d];
        [blit endEncoding];
    }
}

void CommandGraph::copyTextureToBuffer(TextureHandle src, BufferHandle dst) {
    if (!impl_->open) return;
    @autoreleasepool {
        id<MTLTexture> s = impl_->pool->impl().texture(src);
        id<MTLBuffer> d = impl_->pool->impl().buffer(dst);
        if (!s || !d) return;
        auto desc = impl_->pool->textureDesc(src);
        NSUInteger bytesPerRow = desc.width * bytesPerPixel(desc.format);
        id<MTLBlitCommandEncoder> blit = [impl_->commandBuffer blitCommandEncoder];
        blit.label = @"readback";
        [blit copyFromTexture:s
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(desc.width, desc.height, 1)
                            toBuffer:d
                   destinationOffset:0
              destinationBytesPerRow:bytesPerRow
            destinationBytesPerImage:bytesPerRow * desc.height];
        [blit endEncoding];
    }
}

const std::vector<PassTiming>& CommandGraph::lastPassTimings() const {
    return impl_->timer.lastFrameTimings();
}

double CommandGraph::lastFrameGPUms() const { return impl_->lastWholeBufferMs; }

uint32_t CommandGraph::lastFramePassCount() const { return impl_->passCountThisFrame; }

MetalContext& CommandGraph::metal() const { return *impl_->ctx; }
ResourcePool& CommandGraph::resources() const { return *impl_->pool; }

} // namespace life
