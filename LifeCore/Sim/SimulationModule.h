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

    // Print-image pipeline (LifeOfflineRender --dump-state): after the LAST
    // simulated frame, write whatever GPU state a faithful CPU/Python re-draw
    // of that exact frame needs (trail fields, agent positions, the render
    // params actually in effect) into `dir`, and fill `meta` with those
    // params as JSON (SceneRunner::dumpState nests it under the module's
    // instance name in state.json). Default no-op — only modules that
    // contribute pixels to the print image need to implement this.
    virtual void dumpState(SimulationContext& ctx, const std::string& dir,
                           nlohmann::json& meta) {
        (void)ctx; (void)dir; (void)meta;
    }

    // Inverse of dumpState() (LifeRealtime --load-state): read back whatever
    // dumpState() wrote for THIS instance from `dir`, using `meta` (this
    // module's own nested block from state.json, already resolved by
    // SceneRunner::loadState()), into live GPU state. Called once, right
    // after setup()+reset() and before the first encode() — implementations
    // must leave whatever "first encode() does cold-start init" flag they
    // have cleared, so that first encode() does not immediately overwrite
    // the freshly loaded data.
    //
    // Default no-op returning true (nothing to load — matches dumpState()'s
    // default no-op). Return false only for a genuine problem (a capacity
    // that no longer matches the snapshot, a missing/corrupt .npy); the
    // caller treats any false as "abandon the whole warm start, fall back
    // to a cold start" rather than applying a partial load.
    virtual bool loadState(SimulationContext& ctx, const std::string& dir,
                          const nlohmann::json& meta) {
        (void)ctx; (void)dir; (void)meta;
        return true;
    }

    // RGBA16F composited output. Every module — field or particle — must
    // resolve to a texture (§13.1); particles splat, fields color-map.
    virtual TextureHandle outputTexture() const = 0;

    // Named output/input ports for inter-module coupling (Phase 5, design
    // doc §10). SceneRunner resolves "moduleA.portX" -> "moduleB.portY"
    // through these virtual calls — never dynamic_cast type-branching — so
    // adding a new coupled module never touches SceneRunner. Defaults make
    // an unconnected port a harmless no-op: namedOutput() on a port a
    // module doesn't have returns an invalid handle, and bindNamedInput()
    // on a port it doesn't accept returns false; either way the receiving
    // module keeps sampling whatever fallback texture it bound at setup().
    virtual TextureHandle namedOutput(const std::string& port) const { return {}; }
    virtual bool bindNamedInput(const std::string& port, TextureHandle h) {
        (void)port; (void)h; return false;
    }

    // SceneRunner::step normally skips encode() entirely for a layer whose
    // opacity is ~0 (GPU-saving "hidden layers cost nothing" rule — see
    // SceneRunner.cpp). A pure feeder module (e.g. PresenceField driving
    // fluid0.forceField) needs the opposite: its own visibility is only a
    // debug toggle, and whatever it's connected to must keep receiving
    // fresh data every frame regardless. Default false preserves every
    // existing module's exact current behavior; override to true only for
    // a module that must keep simulating while invisible.
    virtual bool alwaysEncode() const { return false; }

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
