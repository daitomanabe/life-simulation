#pragma once
// LifeCore/Sim/SceneRunner.h
// Shared execution core for all runner apps. Realtime and Offline run the
// SAME simulation code through this class — only pacing, input source and
// output policy differ per app (design doc §17.3 / §22-9,10).
//
// Frame flow (§6.2):
//   step(audio):
//     ParameterBus.update(audio)
//     module.updateCPU(audio)          (all modules)
//     graph.beginFrame
//       module.encode                  (simulation passes, substeps inside)
//       composite                      (layers → RGBA16F render target)
//       [readback]                     (only when requested)
//     graph.endFrame(wait)

#include "LifeCore/IO/FrameRecorder.h"
#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/MetalContext.h"
#include "LifeCore/Metal/PipelineCache.h"
#include "LifeCore/Metal/ResourcePool.h"
#include "LifeCore/Params/ParameterBus.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Render/CompositePass.h"
#include "LifeCore/Sim/SimulationModule.h"

#include <memory>
#include <string>
#include <vector>

namespace life {

struct SceneRunnerDesc {
    Scene scene;
    std::string shaderRoot;
    uint32_t substeps = 1;
    bool enableGPUTiming = true;
};

struct StepOptions {
    bool readback = false; // encode a readback of the final frame
    bool waitGPU = true;
};

class SceneRunner {
public:
    static std::unique_ptr<SceneRunner> create(const SceneRunnerDesc& desc,
                                               std::string& outError);
    ~SceneRunner();

    // Deterministic re-init of every module from the scene seed.
    void reset();

    // Advance one frame. dt is the simulation timestep for this frame.
    void step(const AudioFeatureState& audio, float dt, const StepOptions& opts = {});

    TextureHandle outputTexture() const { return renderTarget_; }
    uint32_t frameIndex() const { return frameIndex_; }
    uint32_t width() const { return desc_.scene.width; }
    uint32_t height() const { return desc_.scene.height; }

    // Export the last frame stepped with readback=true.
    bool dumpPNG(const std::string& path, float exposure, std::string& outError);
    bool dumpEXR(const std::string& path, std::string& outError);

    MetalContext& metal() { return *metal_; }
    ResourcePool& resources() { return *resources_; }
    CommandGraph& graph() { return *graph_; }
    ParameterBus& params() { return params_; }
    const Scene& scene() const { return desc_.scene; }
    const std::vector<std::unique_ptr<SimulationModule>>& simModules() const {
        return modules_;
    }

private:
    SceneRunner() = default;

    SceneRunnerDesc desc_;
    std::unique_ptr<MetalContext> metal_;
    std::unique_ptr<ResourcePool> resources_;
    std::unique_ptr<PipelineCache> pipelines_;
    std::unique_ptr<CommandGraph> graph_;
    std::unique_ptr<FrameRecorder> recorder_;
    ParameterBus params_;
    CompositePass composite_;

    std::vector<std::unique_ptr<SimulationModule>> modules_;
    std::vector<BlendMode> layerModes_;
    std::vector<float> layerOpacities_;
    TextureHandle renderTarget_;
    uint32_t frameIndex_ = 0;
    float simTime_ = 0.0f;
};

} // namespace life
