// LifeCore/Sim/SceneRunner.cpp
#include "LifeCore/Sim/SceneRunner.h"

#include "LifeCore/Sim/ModuleFactory.h"

#include <algorithm>

namespace life {

SceneRunner::~SceneRunner() {
    // Stop sinks explicitly (they may hold Metal/AppKit/IPC resources tied
    // to metal_/resources_) before the automatic member teardown below
    // destroys those unique_ptrs in reverse declaration order.
    for (auto& sink : outputSinks_) sink->stop();
}

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
    runner->reseedWasHigh_.assign(runner->modules_.size(), 0);
    runner->visibleWasHigh_.assign(runner->modules_.size(), 0);
    runner->encodedOnce_.assign(runner->modules_.size(), 0);
    runner->layerLiveOpacity_.assign(runner->modules_.size(), 0.0f);

    // Resolve scene.connections into module-pointer pairs once, up front.
    // "lenia0.field" -> name="lenia0", port="field"; no '.' means port
    // defaults to "field". Only the module name is validated here (create
    // fails if either endpoint doesn't exist) — the port is whatever the
    // module makes of it every frame in step() (design point 2/3: no
    // dynamic_cast, unsupported ports are a harmless no-op).
    auto findModule = [&](const std::string& name) -> SimulationModule* {
        for (auto& m : runner->modules_)
            if (m->instanceName() == name) return m.get();
        return nullptr;
    };
    auto splitPort = [](const std::string& s, std::string& name, std::string& port) {
        auto dot = s.find('.');
        if (dot == std::string::npos) {
            name = s;
            port = "field";
        } else {
            name = s.substr(0, dot);
            port = s.substr(dot + 1);
        }
    };
    for (const auto& c : desc.scene.connections) {
        SceneRunner::ResolvedConnection rc;
        std::string srcName, dstName;
        splitPort(c.from, srcName, rc.srcPort);
        splitPort(c.to, dstName, rc.dstPort);
        rc.src = findModule(srcName);
        rc.dst = findModule(dstName);
        if (!rc.src || !rc.dst) {
            outError = "connection references unknown module (\"" + c.from + "\" -> \"" +
                       c.to + "\")";
            return nullptr;
        }
        runner->resolvedConnections_.push_back(rc);
    }

    runner->reset();
    return runner;
}

void SceneRunner::reset() {
    frameIndex_ = 0;
    simTime_ = 0.0f;
    for (auto& m : modules_) m->reset(desc_.scene.seed);
    std::fill(reseedWasHigh_.begin(), reseedWasHigh_.end(), 0);
    std::fill(visibleWasHigh_.begin(), visibleWasHigh_.end(), 0);
    std::fill(encodedOnce_.begin(), encodedOnce_.end(), 0);
    std::fill(layerLiveOpacity_.begin(), layerLiveOpacity_.end(), 0.0f);
}

void SceneRunner::step(const AudioFeatureState& audio, float dt, const StepOptions& opts) {
    MusicFeatureState music;
    music.audio = audio;
    music.frame = frameIndex_;
    step(music, dt, opts);
}

void SceneRunner::step(const MusicFeatureState& music, float dt, const StepOptions& opts) {
    const AudioFeatureState& audio = music.audio;
    params_.update(music, dt);

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

    // Re-resolve every coupling connection immediately before the encode
    // loop, EVERY frame — never once at setup. Ping-pong fields flip which
    // physical texture is their "read" side inside the owning module's own
    // encode(), so a handle captured at setup time would go stale the
    // instant that module swapped for the first time. Binding here always
    // captures each source's state as of the end of the PREVIOUS frame
    // (the source's own encode() for this frame hasn't run yet), so the
    // whole coupled pipeline runs with a deliberate, documented one-frame
    // delay end to end (Phase 5 spec §2) — modules still encode below in
    // scene.modules array order.
    for (auto& c : resolvedConnections_)
        c.dst->bindNamedInput(c.dstPort, c.src->namedOutput(c.srcPort));

    for (size_t i = 0; i < modules_.size(); ++i) {
        SimulationModule* m = modules_[i].get();
        const std::string& n = m->instanceName();

        // このフレームで見えるか。opacity は毎フレーム引くので、musicMappings が
        // セクションごとに「どの生命を見せるか」を切り替えられる（VJ の
        // Composition が phrase ごとにシーンを差し替えるのと同じ考え方）。
        const float op = params_.value(n + ".opacity", layerOpacities_[i]);
        layerLiveOpacity_[i] = op;
        const bool visible = op > VISIBLE_EPS;

        // 隠れた層は encode を飛ばす（見えない生命に GPU を割かない）。5 層を
        // 常時回すと GPU 75ms/frame だが、一度に 1〜2 層しか見せないなら 15ms 台。
        // 生命なので「止めて再開」は嘘になる — 代わりに、再び現れる瞬間に
        // reset() で生まれ直させる。隠れている間の時間経過は演じない。
        const bool appearing = visible && !visibleWasHigh_[i];
        visibleWasHigh_[i] = visible ? 1 : 0;
        if (!visible) continue;

        // reseed: 0.5 を跨いだ立ち上がり、または層が現れた瞬間に再初期化。種は
        // シーン種とフレーム番号から決まるので、同じ入力なら毎回同じ形に生まれ
        // 変わる（frame 0 では seed^0 == seed なので、初期可視の層は reset() の
        // 結果を保つ = 既存プリセットと決定性テストはバイト不変）。
        const bool reseedHigh = params_.value(n + ".reseed", 0.0f) > 0.5f;
        if (appearing || (reseedHigh && !reseedWasHigh_[i])) {
            m->reset(desc_.scene.seed ^ (frameIndex_ * 2654435761u));
            encodedOnce_[i] = 0;
        }
        reseedWasHigh_[i] = reseedHigh ? 1 : 0;

        // freeze: encode を丸ごと飛ばす。出力テクスチャは前フレームの内容を
        // 保ったまま合成される。まだ一度も encode していないモジュールは、
        // 出力テクスチャが未定義なので凍結させない。
        const bool freeze =
            encodedOnce_[i] && params_.value(n + ".freeze", 0.0f) > 0.5f;
        if (freeze) continue;

        m->encode(ctx);
        encodedOnce_[i] = 1;
    }

    std::vector<CompositeLayer> layers;
    layers.reserve(modules_.size());
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (!encodedOnce_[i] || layerLiveOpacity_[i] <= VISIBLE_EPS) continue;
        layers.push_back(
            {modules_[i]->outputTexture(), layerModes_[i], layerLiveOpacity_[i]});
    }
    composite_.encode(*graph_, layers, renderTarget_, desc_.scene.width,
                      desc_.scene.height);

    // sink-less path costs one empty-vector loop check (design doc phase 6:
    // "sink なしの経路にコストゼロ").
    for (auto& sink : outputSinks_) sink->publish(*graph_, renderTarget_);

    if (opts.readback) recorder_->encodeReadback(*graph_, renderTarget_);
    graph_->endFrame(opts.waitGPU);
    if (opts.readback && opts.waitGPU) recorder_->fetch();

    simTime_ += dt;
    frameIndex_++;
}

void SceneRunner::addOutputSink(std::unique_ptr<OutputSink> sink) {
    std::string err;
    if (!sink->start(*metal_, *resources_, desc_.scene.width, desc_.scene.height, err)) {
        fprintf(stderr, "[life] output sink '%s' failed to start: %s\n", sink->name(),
                err.c_str());
        return;
    }
    outputSinks_.push_back(std::move(sink));
}

void SceneRunner::pumpOutputs(bool& shouldQuit) {
    for (auto& sink : outputSinks_) sink->pump(shouldQuit);
}

bool SceneRunner::dumpPNG(const std::string& path, float exposure,
                          std::string& outError) {
    return recorder_->writePNG(path, exposure, outError);
}

bool SceneRunner::dumpEXR(const std::string& path, std::string& outError) {
    return recorder_->writeEXR(path, outError);
}

} // namespace life
