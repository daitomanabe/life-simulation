// LifeCore/Metal/ResourcePool.mm
#include "LifeCore/Metal/MetalInternal.h"

namespace life {

uint32_t bytesPerPixel(PixelFormat f) {
    switch (f) {
        case PixelFormat::R16F: return 2;
        case PixelFormat::R32F: return 4;
        case PixelFormat::RG16F: return 4;
        case PixelFormat::RG32F: return 8;
        case PixelFormat::RGBA16F: return 8;
        case PixelFormat::RGBA32F: return 16;
        case PixelFormat::RGBA8: return 4;
        case PixelFormat::BGRA8Unorm: return 4;
    }
    return 4;
}

const char* pixelFormatName(PixelFormat f) {
    switch (f) {
        case PixelFormat::R16F: return "R16F";
        case PixelFormat::R32F: return "R32F";
        case PixelFormat::RG16F: return "RG16F";
        case PixelFormat::RG32F: return "RG32F";
        case PixelFormat::RGBA16F: return "RGBA16F";
        case PixelFormat::RGBA32F: return "RGBA32F";
        case PixelFormat::RGBA8: return "RGBA8";
        case PixelFormat::BGRA8Unorm: return "BGRA8Unorm";
    }
    return "?";
}

MTLPixelFormat toMTLPixelFormat(PixelFormat f) {
    switch (f) {
        case PixelFormat::R16F: return MTLPixelFormatR16Float;
        case PixelFormat::R32F: return MTLPixelFormatR32Float;
        case PixelFormat::RG16F: return MTLPixelFormatRG16Float;
        case PixelFormat::RG32F: return MTLPixelFormatRG32Float;
        case PixelFormat::RGBA16F: return MTLPixelFormatRGBA16Float;
        case PixelFormat::RGBA32F: return MTLPixelFormatRGBA32Float;
        case PixelFormat::RGBA8: return MTLPixelFormatRGBA8Unorm;
        case PixelFormat::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
    }
    return MTLPixelFormatRGBA16Float;
}

id<MTLTexture> ResourcePool::Impl::texture(TextureHandle h) const {
    if (!h.valid()) return nil;
    uint32_t slot = h.index - 1;
    if (slot >= textures.size()) return nil;
    const auto& s = textures[slot];
    if (!s.alive || s.generation != h.generation) return nil;
    return s.texture;
}

id<MTLBuffer> ResourcePool::Impl::buffer(BufferHandle h) const {
    if (!h.valid()) return nil;
    uint32_t slot = h.index - 1;
    if (slot >= buffers.size()) return nil;
    const auto& s = buffers[slot];
    if (!s.alive || s.generation != h.generation) return nil;
    return s.buffer;
}

ResourcePool::ResourcePool(MetalContext& ctx) : impl_(std::make_unique<Impl>()) {
    impl_->ctx = &ctx;
}
ResourcePool::~ResourcePool() = default;

TextureHandle ResourcePool::createTexture(const TextureDesc& desc) {
    @autoreleasepool {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:toMTLPixelFormat(desc.format)
                                         width:desc.width
                                        height:desc.height
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = desc.storage == StorageMode::Shared ? MTLStorageModeShared
                                                             : MTLStorageModePrivate;
        id<MTLTexture> tex = [impl_->ctx->impl().device newTextureWithDescriptor:td];
        if (!tex) {
            fprintf(stderr, "[life] texture allocation failed: %s (%ux%u %s)\n",
                    desc.label.c_str(), desc.width, desc.height,
                    pixelFormatName(desc.format));
            return {};
        }
        if (!desc.label.empty())
            tex.label = [NSString stringWithUTF8String:desc.label.c_str()];

        // Reuse a dead slot if possible.
        uint32_t slot = UINT32_MAX;
        for (uint32_t i = 0; i < impl_->textures.size(); ++i) {
            if (!impl_->textures[i].alive) { slot = i; break; }
        }
        if (slot == UINT32_MAX) {
            slot = static_cast<uint32_t>(impl_->textures.size());
            impl_->textures.emplace_back();
        }
        auto& s = impl_->textures[slot];
        s.texture = tex;
        s.desc = desc;
        s.alive = true;
        // generation was bumped on release; keep as is on first use
        impl_->textureBytes +=
            size_t(desc.width) * desc.height * bytesPerPixel(desc.format);
        return TextureHandle{slot + 1, s.generation};
    }
}

BufferHandle ResourcePool::createBuffer(const BufferDesc& desc) {
    @autoreleasepool {
        MTLResourceOptions opts = desc.storage == StorageMode::Shared
                                      ? MTLResourceStorageModeShared
                                      : MTLResourceStorageModePrivate;
        id<MTLBuffer> buf = [impl_->ctx->impl().device newBufferWithLength:desc.size
                                                                   options:opts];
        if (!buf) {
            fprintf(stderr, "[life] buffer allocation failed: %s (%zu bytes)\n",
                    desc.label.c_str(), desc.size);
            return {};
        }
        if (!desc.label.empty())
            buf.label = [NSString stringWithUTF8String:desc.label.c_str()];

        uint32_t slot = UINT32_MAX;
        for (uint32_t i = 0; i < impl_->buffers.size(); ++i) {
            if (!impl_->buffers[i].alive) { slot = i; break; }
        }
        if (slot == UINT32_MAX) {
            slot = static_cast<uint32_t>(impl_->buffers.size());
            impl_->buffers.emplace_back();
        }
        auto& s = impl_->buffers[slot];
        s.buffer = buf;
        s.desc = desc;
        s.alive = true;
        impl_->bufferBytes += desc.size;
        return BufferHandle{slot + 1, s.generation};
    }
}

void ResourcePool::release(TextureHandle h) {
    if (!impl_->texture(h)) return;
    auto& s = impl_->textures[h.index - 1];
    impl_->textureBytes -=
        size_t(s.desc.width) * s.desc.height * bytesPerPixel(s.desc.format);
    s.texture = nil;
    s.alive = false;
    s.generation++;
}

void ResourcePool::release(BufferHandle h) {
    if (!impl_->buffer(h)) return;
    auto& s = impl_->buffers[h.index - 1];
    impl_->bufferBytes -= s.desc.size;
    s.buffer = nil;
    s.alive = false;
    s.generation++;
}

bool ResourcePool::isValid(TextureHandle h) const { return impl_->texture(h) != nil; }
bool ResourcePool::isValid(BufferHandle h) const { return impl_->buffer(h) != nil; }

TextureDesc ResourcePool::textureDesc(TextureHandle h) const {
    if (!impl_->texture(h)) return {};
    return impl_->textures[h.index - 1].desc;
}

size_t ResourcePool::bufferSize(BufferHandle h) const {
    if (!impl_->buffer(h)) return 0;
    return impl_->buffers[h.index - 1].desc.size;
}

bool ResourcePool::uploadTexture(TextureHandle h, const void* data,
                                 size_t bytesPerRow) {
    @autoreleasepool {
        id<MTLTexture> tex = impl_->texture(h);
        if (!tex || !data) return false;
        // Private-storage textures cannot be written from CPU; require Shared
        // for uploads (kernel textures are tiny, Shared costs nothing).
        if (tex.storageMode == MTLStorageModePrivate) {
            fprintf(stderr, "[life] uploadTexture: use StorageMode::Shared for %s\n",
                    impl_->textures[h.index - 1].desc.label.c_str());
            return false;
        }
        MTLRegion region = MTLRegionMake2D(0, 0, tex.width, tex.height);
        [tex replaceRegion:region mipmapLevel:0 withBytes:data bytesPerRow:bytesPerRow];
        return true;
    }
}

void* ResourcePool::bufferContents(BufferHandle h) {
    id<MTLBuffer> buf = impl_->buffer(h);
    if (!buf) return nullptr;
    if (impl_->buffers[h.index - 1].desc.storage != StorageMode::Shared) return nullptr;
    return buf.contents;
}

uint32_t ResourcePool::liveTextureCount() const {
    uint32_t n = 0;
    for (auto& s : impl_->textures) n += s.alive ? 1 : 0;
    return n;
}

uint32_t ResourcePool::liveBufferCount() const {
    uint32_t n = 0;
    for (auto& s : impl_->buffers) n += s.alive ? 1 : 0;
    return n;
}

size_t ResourcePool::textureMemoryBytes() const { return impl_->textureBytes; }
size_t ResourcePool::bufferMemoryBytes() const { return impl_->bufferBytes; }

} // namespace life
