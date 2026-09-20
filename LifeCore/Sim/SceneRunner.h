#pragma once
// LifeCore/Sim/SceneRunner.h
// Shared execution core for all runner apps. Realtime and Offline run the
// SAME simulation code through this class — only pacing, input source and
// output policy differ per app (design doc §17.3 / §22-9,10).
//
// Frame flow (§6.2):
//   step(music):
//     ParameterBus.update(music)
//     module.updateCPU(music.audio)    (all modules)
//     graph.beginFrame
//       module.encode                  (simulation passes, substeps inside)
//       composite                      (layers → RGBA16F render target)
//       [readback]                     (only when requested)
//     graph.endFrame(wait)
//
// 予約パラメータ（モジュール側の実装は不要 — ここで解釈する）:
//   "<module>.freeze"  > 0.5 の間、そのモジュールの encode を飛ばす。場は
//                      最後の状態のまま合成され続ける。granular_freeze 用。
//   "<module>.reseed"  0.5 を跨いで立ち上がった瞬間に reset() する。種は
//                      scene.seed とフレーム番号から決まるので再現性がある。
// どちらも musicMappings から駆動できる。SimulationModule に仮想関数を
// 足さずに済むのは、両者が「モジュールを呼ぶか呼ばないか」の話だからだ。

#include "LifeCore/IO/FrameRecorder.h"
#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/MetalContext.h"
#include "LifeCore/Metal/PipelineCache.h"
#include "LifeCore/Metal/ResourcePool.h"
#include "LifeCore/Output/OutputSink.h"
#include "LifeCore/Params/ParameterBus.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Render/CompositePass.h"
#include "LifeCore/Render/PostPass.h"
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
    // 音のみ（従来の呼び出し。構造レーンは全て 0 として扱われる）。
    void step(const AudioFeatureState& audio, float dt, const StepOptions& opts = {});
    // 音 + 楽曲構造。--music を渡したアプリはこちらを呼ぶ。
    void step(const MusicFeatureState& music, float dt, const StepOptions& opts = {});

    TextureHandle outputTexture() const { return renderTarget_; }
    uint32_t frameIndex() const { return frameIndex_; }
    uint32_t width() const { return desc_.scene.width; }
    uint32_t height() const { return desc_.scene.height; }

    // Export the last frame stepped with readback=true.
    bool dumpPNG(const std::string& path, float exposure, std::string& outError);
    bool dumpEXR(const std::string& path, std::string& outError);

    // Print-image pipeline (--dump-state): after the LAST simulated frame,
    // write state.json (scene/frame/fps/seed/bloom/module order + per-module
    // meta), final.npy (raw linear RGBA render target) and every module's
    // own dump (trail fields, agent positions, ...) into `dir`. fps and
    // scenePath are metadata this class doesn't otherwise track (fps is a
    // CLI-only concept; scenePath is recorded as its basename only).
    bool dumpState(const std::string& dir, double fps, const std::string& scenePath,
                   std::string& outError);

    // Register a live output sink (window preview, Syphon, ...). Must be
    // called after create() and before the app's run loop starts stepping.
    // start() runs synchronously here; on failure the sink is discarded
    // (with a stderr warning) rather than aborting the app (design doc §3.8
    // phase 6 §2).
    void addOutputSink(std::unique_ptr<OutputSink> sink);

    // Pump every registered sink once, outside the frame (event pumps,
    // window-close detection, ...). shouldQuit is left untouched unless a
    // sink requests a graceful stop (e.g. the preview window was closed).
    void pumpOutputs(bool& shouldQuit);

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

    // Resolved module.connections entry (Phase 5, design doc §10): pointers
    // into modules_, valid for the runner's lifetime since modules_ never
    // grows/reallocates after create(). Only the module *names* are
    // validated at resolve time; the *ports* are resolved every frame via
    // namedOutput()/bindNamedInput() (see step()), which degrade gracefully
    // if a module doesn't implement the requested port.
    struct ResolvedConnection {
        SimulationModule* src = nullptr;
        std::string srcPort;
        SimulationModule* dst = nullptr;
        std::string dstPort;
    };

    SceneRunnerDesc desc_;
    std::unique_ptr<MetalContext> metal_;
    std::unique_ptr<ResourcePool> resources_;
    std::unique_ptr<PipelineCache> pipelines_;
    std::unique_ptr<CommandGraph> graph_;
    std::unique_ptr<FrameRecorder> recorder_;
    ParameterBus params_;
    CompositePass composite_;
    PostPass post_; // scene-level "post" block (bloom, accumulate); no-op without it

    std::vector<std::unique_ptr<SimulationModule>> modules_;
    std::vector<ResolvedConnection> resolvedConnections_;
    std::vector<BlendMode> layerModes_;
    std::vector<float> layerOpacities_;
    // 予約パラメータの立ち上がり検出 / 「一度も encode していないモジュールは
    // 凍結できない」ためのガード（出力テクスチャがまだ未定義なので）。
    std::vector<uint8_t> reseedWasHigh_;
    std::vector<uint8_t> visibleWasHigh_;  // 隠れ→表示の立ち上がりで生まれ直す
    std::vector<uint8_t> encodedOnce_;
    std::vector<float> layerLiveOpacity_;  // このフレームの実 opacity（合成で再利用）
    static constexpr float VISIBLE_EPS = 0.02f;
    TextureHandle renderTarget_;
    uint32_t frameIndex_ = 0;
    float simTime_ = 0.0f;

    // Declared last so it is destroyed (and stop()'d) first — before metal_/
    // resources_/graph_ above tear down the Metal objects sinks depend on.
    std::vector<std::unique_ptr<OutputSink>> outputSinks_;
};

} // namespace life
