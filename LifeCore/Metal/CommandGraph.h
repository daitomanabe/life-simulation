#pragma once
// LifeCore/Metal/CommandGraph.h
// Owns the per-frame command buffer. Simulation modules never commit command
// buffers themselves — they only describe compute passes here (design doc
// §6.2). One pass == one compute encoder, which also gives us per-pass GPU
// timing and debug labels for free.

#include "LifeCore/Metal/Handles.h"
#include "LifeCore/Metal/GPUTimer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace life {

class MetalContext;
class ResourcePool;
class PipelineCache;
class CommandGraph;

// Describe-then-dispatch helper. All binds are recorded; the actual encoder is
// created and closed inside dispatch2D/dispatch1D, so a pass can never leak an
// open encoder.
class ComputePass {
public:
    ComputePass& pipeline(std::string functionName);
    ComputePass& read(int index, TextureHandle h);   // bound via setTexture
    ComputePass& write(int index, TextureHandle h);  // bound via setTexture
    ComputePass& buffer(int index, BufferHandle h, size_t offset = 0);
    ComputePass& bytes(int index, const void* data, size_t size);

    template <typename T>
    ComputePass& uniforms(int index, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>,
                      "uniform structs must be trivially copyable");
        return bytes(index, &value, sizeof(T));
    }

    // Dispatch over a w×h grid (uses non-uniform threadgroups; Apple Silicon).
    void dispatch2D(uint32_t width, uint32_t height);
    void dispatch1D(uint32_t count);

private:
    friend class CommandGraph;
    explicit ComputePass(CommandGraph& graph, std::string label);

    struct TextureBind { int index; TextureHandle handle; };
    struct BufferBind { int index; BufferHandle handle; size_t offset; };
    struct ByteBind { int index; std::vector<uint8_t> data; };

    CommandGraph& graph_;
    std::string label_;
    std::string function_;
    std::vector<TextureBind> textures_;
    std::vector<BufferBind> buffers_;
    std::vector<ByteBind> bytes_;
};

class CommandGraph {
public:
    CommandGraph(MetalContext& ctx, ResourcePool& pool, PipelineCache& pipelines);
    ~CommandGraph();

    CommandGraph(const CommandGraph&) = delete;
    CommandGraph& operator=(const CommandGraph&) = delete;

    // Frame lifecycle. beginFrame creates the command buffer; endFrame commits
    // it. Offline rendering always waits; realtime currently waits too (MVP —
    // triple buffering is a later optimization).
    void beginFrame(uint32_t frameIndex);
    void endFrame(bool waitUntilCompleted = true);
    bool frameOpen() const;

    ComputePass pass(std::string label) { return ComputePass(*this, std::move(label)); }

    // Blit helpers (each runs in its own blit encoder).
    void copyTexture(TextureHandle src, TextureHandle dst);
    void copyTextureToBuffer(TextureHandle src, BufferHandle dst);

    // Timing for the last completed frame.
    const std::vector<PassTiming>& lastPassTimings() const;
    double lastFrameGPUms() const;
    uint32_t lastFramePassCount() const;

    MetalContext& metal() const;
    ResourcePool& resources() const;

    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    friend class ComputePass;
    void dispatch(ComputePass& pass, uint32_t gridW, uint32_t gridH, uint32_t gridD);

    std::unique_ptr<Impl> impl_;
};

} // namespace life
