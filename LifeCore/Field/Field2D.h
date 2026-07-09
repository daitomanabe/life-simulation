#pragma once
// LifeCore/Field/Field2D.h
// A named 2D field: dimensions, format, boundary mode, and (optionally
// ping-ponged) texture storage. Field simulations (RD / Lenia / CA / trails)
// build on this. Boundary handling itself happens in the shaders — Wrap is
// the default because toroidal worlds look best for VJ use (design doc §7.5).

#include "LifeCore/Field/PingPongTexture.h"
#include "LifeCore/Metal/ResourcePool.h"

#include <string>

namespace life {

enum class BoundaryMode : uint32_t {
    Wrap = 0,
    Clamp = 1,
    Mirror = 2,
    Zero = 3,
};

struct Field2DDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::R16F;
    bool pingPong = true;
    BoundaryMode boundary = BoundaryMode::Wrap;
    std::string label;
};

class Field2D {
public:
    Field2D() = default;

    bool create(ResourcePool& pool, const Field2DDesc& desc) {
        pool_ = &pool;
        desc_ = desc;
        TextureDesc td;
        td.width = desc.width;
        td.height = desc.height;
        td.format = desc.format;
        td.storage = StorageMode::GPUPrivate;
        td.label = desc.label;
        if (desc.pingPong) {
            return pingPong_.create(pool, td);
        }
        single_ = pool.createTexture(td);
        return single_.valid();
    }

    void destroy() {
        if (!pool_) return;
        pingPong_.destroy();
        if (single_.valid()) pool_->release(single_);
        single_ = {};
    }

    const Field2DDesc& desc() const { return desc_; }
    uint32_t width() const { return desc_.width; }
    uint32_t height() const { return desc_.height; }
    BoundaryMode boundary() const { return desc_.boundary; }

    TextureHandle read() const { return desc_.pingPong ? pingPong_.read() : single_; }
    TextureHandle write() const { return desc_.pingPong ? pingPong_.write() : single_; }
    void swap() { if (desc_.pingPong) pingPong_.swap(); }
    bool valid() const { return desc_.pingPong ? pingPong_.valid() : single_.valid(); }

private:
    ResourcePool* pool_ = nullptr;
    Field2DDesc desc_;
    PingPongTexture pingPong_;
    TextureHandle single_;
};

} // namespace life
