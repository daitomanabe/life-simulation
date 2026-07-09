// LifeCore/Sim/SceneRunner.cpp
#include "LifeCore/Sim/SceneRunner.h"

#include "LifeCore/Sim/ModuleFactory.h"

namespace life {

SceneRunner::~SceneRunner() = default;

std::unique_ptr<SceneRunner> SceneRunner::create(const SceneRunnerDesc& desc,
                                                 std::string& outError) {
    std::unique_ptr<SceneRunner> runner(new SceneRunner());
    runner->desc_ = desc;

    MetalContextDesc mc;
    mc.shaderRoot = desc.shaderRoot;
    mc.enableGPUTiming = desc.enableGPUTiming;
    runner->metal_ = MetalContext::create(mc, outError);
    if (!runner->metal_) return nullptr;

    runner->resources_ = std::make_unique<ResourcePool>(*runner->metal_);
    runner->pipelines_ = std::make_unique<PipelineCache>(*runner->metal_);
    runner->graph_ = std::make_unique<CommandGraph>(*runner->metal_,
                                                    *runner->resources_,
                                                    *runner->pipelines_);
    runner->recorder_ = std::make_unique<FrameRecorder>(*runner->resources_);

    // Final render target — the one texture every output path reads.
    TextureDesc rt;
    rt.width = desc.scene.width;
    rt.height = desc.scene.height;
    rt.format = PixelFormat::RGBA16F;
    rt.label = "renderTarget";
    runner->renderTarget_ = runner->resources_->createTexture(rt);
    if (!runner->renderTarget_.valid()) {
        outError = "failed to create render target";
        return nullptr;
    }

    // Instantiate scene modules through the factory.
    desc.scene.applyBaseParams(runner->params_);
    for (const auto& spec : desc.scene.modules) {
        if (!spec.enabled) continue;
        auto module = ModuleFactory::instance().create(spec.type);
        if (!module) {
            outError = "unknown module type: " + spec.type;
            return nullptr;
        }
        module->configure(spec.name, spec.params);

        SimulationContext ctx;
        ctx.metal = runner->metal_.get();
        ctx.resources = runner->resources_.get();
        ctx.params = &runner->params_;
        ctx.graph = runner->graph_.get();
        ctx.width = desc.scene.width;
        ctx.height = desc.scene.height;
        ctx.substeps = desc.substeps;
        module->setup(ctx);

        runner->layerModes_.push_back(
            blendModeFromString(spec.params.value("blend", "add")));
        runner->layerOpacities_.push_back(spec.params.value("opacity", 1.0f));
        runner->modules_.push_back(std::move(module));
    }
    if (runner->modules_.empty()) {
        outError = "scene contains no enabled modules";
        return nullptr;
    }

    runner->reset();
    return runner;
}

void SceneRunner::reset() {
    frameIndex_ = 0;
    simTime_ = 0.0f;
    for (auto& m : modules_) m->reset(desc_.scene.seed);
}

void SceneRunner::step(const AudioFeatureState& audio, float dt, const StepOptions& opts) {
    params_.update(audio, dt);

    SimulationContext ctx;
    ctx.metal = metal_.get();
    ctx.resources = resources_.get();
    ctx.params = &params_;
    ctx.graph = graph_.get();
    ctx.frameIndex = frameIndex_;
    ctx.substeps = desc_.substeps;
    ctx.dt = dt;
    ctx.width = desc_.scene.width;
    ctx.height = desc_.scene.height;

    for (auto& m : modules_) m->updateCPU(audio);

    graph_->beginFrame(frameIndex_);
    for (auto& m : modules_) m->encode(ctx);

    std::vector<CompositeLayer> layers;
    layers.reserve(modules_.size());
    for (size_t i = 0; i < modules_.size(); ++i) {
        layers.push_back(
            {modules_[i]->outputTexture(), layerModes_[i], layerOpacities_[i]});
    }
    composite_.encode(*graph_, layers, renderTarget_, desc_.scene.width,
                      desc_.scene.height);

    if (opts.readback) recorder_->encodeReadback(*graph_, renderTarget_);
    graph_->endFrame(opts.waitGPU);
    if (opts.readback && opts.waitGPU) recorder_->fetch();

    simTime_ += dt;
    frameIndex_++;
}

bool SceneRunner::dumpPNG(const std::string& path, float exposure,
                          std::string& outError) {
    return recorder_->writePNG(path, exposure, outError);
}

bool SceneRunner::dumpEXR(const std::string& path, std::string& outError) {
    return recorder_->writeEXR(path, outError);
}

} // namespace life
