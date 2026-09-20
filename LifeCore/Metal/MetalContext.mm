// LifeCore/Metal/MetalContext.mm
#include "LifeCore/Metal/MetalInternal.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace life {

static bool readTextFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Collect .metal sources: Common/ first (shared types & helpers), then the
// remaining directories, each sorted by path for deterministic concatenation.
static std::vector<fs::path> collectShaderSources(const fs::path& root) {
    std::vector<fs::path> common, rest;
    if (!fs::exists(root)) return {};
    for (auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".metal") continue;
        auto rel = fs::relative(entry.path(), root);
        if (!rel.empty() && rel.begin()->string() == "Common") {
            common.push_back(entry.path());
        } else {
            rest.push_back(entry.path());
        }
    }
    std::sort(common.begin(), common.end());
    std::sort(rest.begin(), rest.end());
    common.insert(common.end(), rest.begin(), rest.end());
    return common;
}

bool MetalContext::Impl::compileLibrary(std::string& outError) {
    auto sources = collectShaderSources(shaderRoot);
    if (sources.empty()) {
        outError = "no .metal sources found under " + shaderRoot;
        return false;
    }

    std::string merged;
    merged.reserve(1 << 16);
    for (auto& p : sources) {
        std::string text;
        if (!readTextFile(p, text)) {
            outError = "failed to read shader source " + p.string();
            return false;
        }
        merged += "// ---- ";
        merged += p.filename().string();
        merged += " ----\n";
        merged += text;
        merged += "\n";
    }

    @autoreleasepool {
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
#ifdef LIFE_DETERMINISTIC_MATH
        // Fast math is ON by default, and it lets the Metal backend choose FMA
        // contraction and approximate transcendentals per GPU generation. That
        // is fine on one machine, but a replicated multi-machine render needs
        // the same bits on every node. Turning it off costs about 16% of the
        // frame and CHANGES the picture slightly, so it is opt-in:
        //   cmake -B build -DLIFE_DETERMINISTIC_MATH=ON
        if (@available(macOS 15.0, *)) {
            options.mathMode = MTLMathModeSafe;
        } else {
            options.fastMathEnabled = NO;
        }
#endif
        NSError* error = nil;
        NSString* src = [NSString stringWithUTF8String:merged.c_str()];
        id<MTLLibrary> lib = [device newLibraryWithSource:src options:options error:&error];
        if (!lib) {
            outError = error ? std::string([[error localizedDescription] UTF8String])
                             : "unknown shader compile error";
            return false;
        }
        if (error) {
            // Compile succeeded with warnings; surface them once on stderr.
            fprintf(stderr, "[life] shader warnings:\n%s\n",
                    [[error localizedDescription] UTF8String]);
        }
        library = lib;
    }
    return true;
}

MetalContext::MetalContext() : impl_(std::make_unique<Impl>()) {}
MetalContext::~MetalContext() = default;

std::unique_ptr<MetalContext> MetalContext::create(const MetalContextDesc& desc,
                                                   std::string& outError) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            outError = "MTLCreateSystemDefaultDevice() returned nil (no Metal GPU)";
            return nullptr;
        }
        std::unique_ptr<MetalContext> ctx(new MetalContext());
        ctx->impl_->device = device;
        ctx->impl_->queue = [device newCommandQueue];
        ctx->impl_->queue.label = @"LifeCore";
        ctx->impl_->shaderRoot = desc.shaderRoot;
        ctx->impl_->enableGPUTiming = desc.enableGPUTiming;
        if (!ctx->impl_->compileLibrary(outError)) return nullptr;
        return ctx;
    }
}

std::string MetalContext::deviceName() const {
    return std::string([impl_->device.name UTF8String]);
}

const std::string& MetalContext::shaderRoot() const { return impl_->shaderRoot; }

bool MetalContext::gpuTimingSupported() const { return impl_->timingSupported; }

bool MetalContext::reloadShaders(std::string& outError) {
    return impl_->compileLibrary(outError);
}

} // namespace life
