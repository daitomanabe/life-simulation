#pragma once
// LifeCore/Metal/ResourcePool.h
// Central owner of all GPU textures and buffers. Simulation modules must not
// create Metal resources themselves (design doc §6.4 / §22-5,6); they request
// them here and receive opaque handles. This makes GPU memory measurable and
// prevents leaks when modules are added or reset.

#include "LifeCore/Metal/Handles.h"

#include <cstddef>
#include <string>

namespace life {

class MetalContext;

enum class PixelFormat {
    R16F,
    R32F,
    RG16F,
    RG32F,
    RGBA16F,
    RGBA32F,
    RGBA8,
};

uint32_t bytesPerPixel(PixelFormat f);
const char* pixelFormatName(PixelFormat f);

enum class StorageMode {
    GPUPrivate, // GPU-only (simulation state, render targets)
    Shared,     // CPU-visible (readback, upload)
};

struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::RGBA16F;
    StorageMode storage = StorageMode::GPUPrivate;
    std::string label;
};

struct BufferDesc {
    size_t size = 0;
    StorageMode storage = StorageMode::Shared;
    std::string label;
};

class ResourcePool {
public:
    explicit ResourcePool(MetalContext& ctx);
    ~ResourcePool();

    ResourcePool(const ResourcePool&) = delete;
    ResourcePool& operator=(const ResourcePool&) = delete;

    TextureHandle createTexture(const TextureDesc& desc);
    BufferHandle createBuffer(const BufferDesc& desc);

    void release(TextureHandle h);
    void release(BufferHandle h);

    bool isValid(TextureHandle h) const;
    bool isValid(BufferHandle h) const;

    TextureDesc textureDesc(TextureHandle h) const;
    size_t bufferSize(BufferHandle h) const;

    // CPU pointer for Shared buffers (nullptr otherwise).
    void* bufferContents(BufferHandle h);

    // CPU → texture upload (kernel textures, LUTs). Works regardless of
    // storage mode; data is tightly packed rows of width*bytesPerPixel.
    bool uploadTexture(TextureHandle h, const void* data, size_t bytesPerRow);

    // Introspection for LifeBench / leak checks.
    uint32_t liveTextureCount() const;
    uint32_t liveBufferCount() const;
    size_t textureMemoryBytes() const;
    size_t bufferMemoryBytes() const;

    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace life
