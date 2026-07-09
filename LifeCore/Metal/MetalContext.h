#pragma once
// LifeCore/Metal/MetalContext.h
// Owns MTLDevice / MTLCommandQueue / MTLLibrary. Headless by design: no
// window, no CAMetalLayer. Shaders are compiled at runtime from the .metal
// sources under shaderRoot (this toolchain has no offline `xcrun metal`
// compiler, and runtime compilation also enables hot reload later).

#include <memory>
#include <string>

namespace life {

struct MetalContextDesc {
    // Directory containing Common/, Field/, Render/, ... .metal sources.
    std::string shaderRoot;
    bool enableGPUTiming = true;
};

class MetalContext {
public:
    // Returns nullptr (with message in outError) if no Metal device exists or
    // shader compilation fails.
    static std::unique_ptr<MetalContext> create(const MetalContextDesc& desc,
                                                std::string& outError);
    ~MetalContext();

    MetalContext(const MetalContext&) = delete;
    MetalContext& operator=(const MetalContext&) = delete;

    std::string deviceName() const;
    const std::string& shaderRoot() const;
    bool gpuTimingSupported() const;

    // Recompile all shaders from shaderRoot (hot reload). Returns false and
    // keeps the previous library on error.
    bool reloadShaders(std::string& outError);

    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    MetalContext();
    std::unique_ptr<Impl> impl_;
};

} // namespace life
