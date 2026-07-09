// LifeCore/Metal/PipelineCache.mm
#include "LifeCore/Metal/MetalInternal.h"

namespace life {

id<MTLComputePipelineState> PipelineCache::Impl::pipeline(const std::string& name,
                                                          std::string& outError) {
    auto it = pipelines.find(name);
    if (it != pipelines.end()) return it->second;

    @autoreleasepool {
        NSString* fn = [NSString stringWithUTF8String:name.c_str()];
        id<MTLFunction> function = [ctx->impl().library newFunctionWithName:fn];
        if (!function) {
            outError = "kernel function not found: " + name;
            return nil;
        }
        NSError* error = nil;
        id<MTLComputePipelineState> pso =
            [ctx->impl().device newComputePipelineStateWithFunction:function error:&error];
        if (!pso) {
            outError = "pipeline creation failed for " + name +
                       (error ? ": " + std::string([[error localizedDescription] UTF8String])
                              : "");
            return nil;
        }
        pipelines.emplace(name, pso);
        return pso;
    }
}

PipelineCache::PipelineCache(MetalContext& ctx) : impl_(std::make_unique<Impl>()) {
    impl_->ctx = &ctx;
}
PipelineCache::~PipelineCache() = default;

bool PipelineCache::hasFunction(const std::string& functionName) {
    std::string err;
    return impl_->pipeline(functionName, err) != nil;
}

void PipelineCache::invalidate() { impl_->pipelines.clear(); }

uint32_t PipelineCache::cachedCount() const {
    return static_cast<uint32_t>(impl_->pipelines.size());
}

} // namespace life
