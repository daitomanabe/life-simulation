#pragma once
// LifeCore/Sim/SimulationModule.h
// The module interface every simulation implements (design doc §11). Field
// and particle simulations need different resources, so there are typed
// sub-interfaces, but the lifecycle is shared:
//
//   setup(ctx)         — create GPU resources via ctx.resources (once)
//   reset(seed)        — deterministic re-initialization (encoded next frame)
//   updateCPU(audio)   — pull final parameter values from ctx.params
//   encode(ctx)        — record compute passes into ctx.graph (NO commits)
//   outputTexture()    — RGBA16F visual output for the compositor (§13.1)

#include "LifeCore/Audio/AudioFeatureState.h"
#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/Handles.h"
#include "LifeCore/Metal/MetalContext.h"
#include "LifeCore/Metal/ResourcePool.h"
#include "LifeCore/Params/ParameterBus.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace life {

struct SimulationContext {
    MetalContext* metal = nullptr;
    ResourcePool* resources = nullptr;
    ParameterBus* params = nullptr;
    CommandGraph* graph = nullptr;
    uint32_t frameIndex = 0;
    uint32_t substeps = 1;    // offline quality knob (§17)
    float dt = 1.0f / 60.0f;  // fixed in offline mode, measured in realtime
    uint32_t width = 0;       // scene resolution
    uint32_t height = 0;
};

class SimulationModule {
public:
    virtual ~SimulationModule() = default;

    virtual void setup(SimulationContext& ctx) = 0;
    virtual void reset(uint32_t seed) = 0;
    virtual void updateCPU(const AudioFeatureState& audio) = 0;
    virtual void encode(SimulationContext& ctx) = 0;

    // RGBA16F composited output. Every module — field or particle — must
    // resolve to a texture (§13.1); particles splat, fields color-map.
    virtual TextureHandle outputTexture() const = 0;

    const std::string& instanceName() const { return instanceName_; }
    bool enabled() const { return enabled_; }
    void setEnabled(bool e) { enabled_ = e; }

    // Called by the factory before setup().
    void configure(std::string instanceName, nlohmann::json params) {
        instanceName_ = std::move(instanceName);
        params_ = std::move(params);
    }
    const nlohmann::json& jsonParams() const { return params_; }

protected:
    // Convenience: read this instance's final (audio-modulated) parameter.
    float param(const SimulationContext& ctx, const std::string& key,
                float fallback) const {
        return ctx.params->value(instanceName_ + "." + key, fallback);
    }

    std::string instanceName_;
    nlohmann::json params_;
    bool enabled_ = true;
};

// Field simulations additionally expose their raw state field so couplers can
// sample it (Phase 5).
class FieldModule : public SimulationModule {
public:
    virtual TextureHandle outputField() const = 0;
};

// Particle simulations expose their particle buffers (Phase 3+).
struct ParticleSetHandle {
    BufferHandle position;
    BufferHandle velocity;
    BufferHandle species;
    BufferHandle attributes;
    uint32_t count = 0;
};

class ParticleModule : public SimulationModule {
public:
    virtual ParticleSetHandle particles() const = 0;
};

class CoupledModule : public SimulationModule {
public:
    virtual void bindFieldInput(TextureHandle field) = 0;
    virtual void bindParticleInput(const ParticleSetHandle& particles) = 0;
};

} // namespace life
