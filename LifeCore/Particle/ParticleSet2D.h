#pragma once
// LifeCore/Particle/ParticleSet2D.h
// SoA particle buffer bundle (design doc §8.2): positions / velocities /
// species / attributes / random-state, all GPUPrivate. Header-only, same
// convention as LifeCore/Field/Field2D.h — modules own one of these and pull
// every GPU resource through ResourcePool (no raw Metal here).
//
// Buffer sizes (all label "<label>.xxx"):
//   positions / velocities : float2 × capacity            (capacity * 8)
//   species                : uint   × capacity            (capacity * 4)
//   attributes              : ParticleAttributes × capacity (capacity * 20)
//                              — age, life, mass, radius (float) + flags
//                              (uint32_t), design doc §8.2.
//   randomState             : uint  × capacity            (capacity * 4)
//
// pingPongPosVel is for particle-interaction sims that must read the whole
// previous-frame set while writing the next one (Phase 4 Particle Life /
// Boids). Phase 3's Slime Mold agents only ever read/write their own lane,
// so it stays false and positions()/positionsWrite() alias the same buffer.

#include "LifeCore/Metal/ResourcePool.h"
#include "LifeCore/Sim/SimulationModule.h"

#include <string>

namespace life {

struct ParticleSet2DDesc {
    uint32_t capacity = 0;
    bool pingPongPosVel = false; // particle-interaction sims (Phase 4) need true
    std::string label;
};

class ParticleSet2D {
public:
    ParticleSet2D() = default;

    bool create(ResourcePool& pool, const ParticleSet2DDesc& desc) {
        pool_ = &pool;
        desc_ = desc;

        BufferDesc bd;
        bd.storage = StorageMode::GPUPrivate;

        bd.size = size_t(desc.capacity) * 8; // float2
        bd.label = desc.label + ".pos.a";
        posA_ = pool.createBuffer(bd);
        if (desc.pingPongPosVel) {
            bd.label = desc.label + ".pos.b";
            posB_ = pool.createBuffer(bd);
        }

        bd.label = desc.label + ".vel.a";
        velA_ = pool.createBuffer(bd);
        if (desc.pingPongPosVel) {
            bd.label = desc.label + ".vel.b";
            velB_ = pool.createBuffer(bd);
        }

        bd.size = size_t(desc.capacity) * 4; // uint
        bd.label = desc.label + ".species";
        species_ = pool.createBuffer(bd);

        bd.label = desc.label + ".random";
        random_ = pool.createBuffer(bd);

        bd.size = size_t(desc.capacity) * 20; // ParticleAttributes (§8.2)
        bd.label = desc.label + ".attributes";
        attributes_ = pool.createBuffer(bd);

        flipped_ = false;
        return valid();
    }

    void destroy() {
        if (!pool_) return;
        pool_->release(posA_);
        pool_->release(posB_);
        pool_->release(velA_);
        pool_->release(velB_);
        pool_->release(species_);
        pool_->release(attributes_);
        pool_->release(random_);
        posA_ = posB_ = velA_ = velB_ = species_ = attributes_ = random_ = {};
    }

    bool valid() const {
        bool base = posA_.valid() && velA_.valid() && species_.valid() &&
                    attributes_.valid() && random_.valid();
        if (!desc_.pingPongPosVel) return base;
        return base && posB_.valid() && velB_.valid();
    }

    // pingPong disabled: read/write alias the same handle (§ spec).
    BufferHandle positions() const { return flipped_ ? posB_ : posA_; }
    BufferHandle positionsWrite() const {
        return desc_.pingPongPosVel ? (flipped_ ? posA_ : posB_) : positions();
    }
    BufferHandle velocities() const { return flipped_ ? velB_ : velA_; }
    BufferHandle velocitiesWrite() const {
        return desc_.pingPongPosVel ? (flipped_ ? velA_ : velB_) : velocities();
    }
    void swap() {
        if (desc_.pingPongPosVel) flipped_ = !flipped_;
    }

    BufferHandle species() const { return species_; }       // uint × capacity
    BufferHandle attributes() const { return attributes_; } // ParticleAttributes × capacity
    BufferHandle randomState() const { return random_; }    // uint × capacity
    uint32_t capacity() const { return desc_.capacity; }

    // Packs the read side into the type SimulationModule.h's ParticleModule
    // interface exposes.
    ParticleSetHandle handle() const {
        ParticleSetHandle h;
        h.position = positions();
        h.velocity = velocities();
        h.species = species_;
        h.attributes = attributes_;
        h.count = desc_.capacity;
        return h;
    }

private:
    ResourcePool* pool_ = nullptr;
    ParticleSet2DDesc desc_;
    BufferHandle posA_, posB_;
    BufferHandle velA_, velB_;
    BufferHandle species_, attributes_, random_;
    bool flipped_ = false;
};

} // namespace life
