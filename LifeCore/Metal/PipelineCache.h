#pragma once
// LifeCore/Metal/PipelineCache.h
// Caches MTLComputePipelineState by kernel function name so the same pipeline
// is never rebuilt (design doc §6.3). Invalidated on shader hot reload.

#include <memory>
#include <string>

namespace life {

class MetalContext;

class PipelineCache {
public:
    explicit PipelineCache(MetalContext& ctx);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // True if the kernel function exists in the current library (compiles the
    // pipeline as a side effect).
    bool hasFunction(const std::string& functionName);

    // Drop all cached pipelines (call after MetalContext::reloadShaders).
    void invalidate();

    uint32_t cachedCount() const;

    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace life
