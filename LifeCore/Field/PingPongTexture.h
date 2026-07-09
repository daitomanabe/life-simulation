#pragma once
// LifeCore/Field/PingPongTexture.h
// Double-buffered texture pair for iterative field simulations. read() is the
// current state, write() the next; swap() after each simulation step.

#include "LifeCore/Metal/ResourcePool.h"

namespace life {

class PingPongTexture {
public:
    PingPongTexture() = default;

    bool create(ResourcePool& pool, const TextureDesc& desc) {
        pool_ = &pool;
        TextureDesc d = desc;
        d.label = desc.label + ".a";
        a_ = pool.createTexture(d);
        d.label = desc.label + ".b";
        b_ = pool.createTexture(d);
        flipped_ = false;
        return a_.valid() && b_.valid();
    }

    void destroy() {
        if (!pool_) return;
        pool_->release(a_);
        pool_->release(b_);
        a_ = b_ = {};
    }

    TextureHandle read() const { return flipped_ ? b_ : a_; }
    TextureHandle write() const { return flipped_ ? a_ : b_; }
    void swap() { flipped_ = !flipped_; }
    bool valid() const { return a_.valid() && b_.valid(); }

private:
    ResourcePool* pool_ = nullptr;
    TextureHandle a_, b_;
    bool flipped_ = false;
};

} // namespace life
