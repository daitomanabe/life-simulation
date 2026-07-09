// Apps/LifeRealtime/main.cpp
// Headless VJ runner (design doc §4.2 / §16): OSC in, 60 fps pacing, status
// line once per second, optional periodic frame dump and audio capture
// recording. No UI, no window — output abstraction (Syphon/NDI) comes later.

#include "Apps/Common/AppCommon.h"
#include "LifeCore/Audio/AudioInput.h"
#include "LifeCore/IO/CaptureReplay.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Sim/ModuleFactory.h"
#include "LifeCore/Sim/SceneRunner.h"

#include <CLI11/CLI11.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

using namespace life;

static std::atomic<bool> gRunning{true};

static void onSignal(int) { gRunning.store(false); }

int main(int argc, char** argv) {
    CLI::App app{"LifeRealtime — headless realtime VJ runner"};

    std::string scenePath = "Presets/default.json";
    std::string shaderRoot;
    std::string recordPath;
    std::string dumpDir = "frames";
    int width = -1, height = -1;
    double fps = 60.0;
    uint16_t oscPort = 9000;
    int64_t seed = -1;
    uint32_t dumpEvery = 0; // 0 = never
    double duration = 0.0;  // 0 = run until SIGINT
    float exposure = 1.0f;

    app.add_option("--scene", scenePath, "Scene JSON path");
    app.add_option("--shaders", shaderRoot, "Shaders/ directory");
    app.add_option("--width", width, "Override scene width");
    app.add_option("--height", height, "Override scene height");
    app.add_option("--fps", fps, "Target frame rate");
    app.add_option("--osc-port", oscPort, "OSC listen port");
    app.add_option("--seed", seed, "Override scene seed");
    app.add_option("--dump-every", dumpEvery, "Dump a PNG preview every N frames");
    app.add_option("--dump-dir", dumpDir, "Preview dump directory");
    app.add_option("--record", recordPath, "Record AudioFeatureState to .jsonl");
    app.add_option("--duration", duration, "Stop after N seconds (0 = until SIGINT)");
    app.add_option("--exposure", exposure, "Preview PNG exposure");
    CLI11_PARSE(app, argc, argv);

    modules::registerBuiltinModules();

    std::string err;
    auto scene = Scene::loadFile(scenePath, err);
    if (!scene) {
        fprintf(stderr, "[life] %s\n", err.c_str());
        return 1;
    }
    if (width > 0) scene->width = uint32_t(width);
    if (height > 0) scene->height = uint32_t(height);
    if (seed >= 0) scene->seed = uint32_t(seed);

    SceneRunnerDesc rd;
    rd.scene = *scene;
    rd.shaderRoot = app::resolveShaderRoot(shaderRoot, argv[0]);
    auto runner = SceneRunner::create(rd, err);
    if (!runner) {
        fprintf(stderr, "[life] %s\n", err.c_str());
        return 1;
    }

    AudioInput audio;
    if (!audio.start(oscPort, err)) {
        fprintf(stderr, "[life] %s\n", err.c_str());
        return 1;
    }

    CaptureWriter capture;
    if (!recordPath.empty()) {
        if (!capture.open(recordPath, err)) {
            fprintf(stderr, "[life] %s\n", err.c_str());
            return 1;
        }
    }
    if (dumpEvery > 0 && !app::ensureDirectory(dumpDir, err)) {
        fprintf(stderr, "[life] %s\n", err.c_str());
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    fprintf(stderr,
            "[life] LifeRealtime | device %s | %ux%u @ %.0ffps | OSC :%u | scene %s\n",
            runner->metal().deviceName().c_str(), scene->width, scene->height, fps,
            oscPort, scenePath.c_str());

    using clock = std::chrono::steady_clock;
    const auto frameInterval =
        std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
    // Fixed sim dt keeps live behavior reproducible when replayed offline.
    const float dt = float(1.0 / fps);

    auto start = clock::now();
    auto nextFrame = start;
    auto lastStatus = start;
    uint32_t framesSinceStatus = 0;
    double gpuMsSinceStatus = 0.0;
    uint64_t lastPackets = 0;

    while (gRunning.load()) {
        auto now = clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (duration > 0.0 && elapsed >= duration) break;

        uint32_t frame = runner->frameIndex();
        const AudioFeatureState& state = audio.update(dt, float(elapsed), frame);
        if (capture.isOpen()) capture.writeFrame(state);

        bool dump = dumpEvery > 0 && frame % dumpEvery == 0;
        StepOptions opts;
        opts.readback = dump;
        runner->step(state, dt, opts);
        gpuMsSinceStatus += runner->graph().lastFrameGPUms();
        framesSinceStatus++;

        if (dump) {
            std::string path = app::frameFilename(dumpDir, "preview", frame, "png");
            if (!runner->dumpPNG(path, exposure, err))
                fprintf(stderr, "[life] dump failed: %s\n", err.c_str());
        }

        // Status once per second (fps, GPU load, OSC traffic, passes).
        now = clock::now();
        if (now - lastStatus >= std::chrono::seconds(1)) {
            double statusSec = std::chrono::duration<double>(now - lastStatus).count();
            auto stats = audio.oscStats();
            fprintf(stderr,
                    "[life] fps %5.1f | gpu %5.2fms | passes %2u | osc %4llu pkt/s | "
                    "kick %.2f snare %.2f hihat %.2f beat %.2f rms %.2f\n",
                    framesSinceStatus / statusSec,
                    gpuMsSinceStatus / std::max(1u, framesSinceStatus),
                    runner->graph().lastFramePassCount(),
                    (unsigned long long)(stats.packetsReceived - lastPackets),
                    state.kick, state.snare, state.hihat, state.beat, state.rms);
            lastPackets = stats.packetsReceived;
            lastStatus = now;
            framesSinceStatus = 0;
            gpuMsSinceStatus = 0.0;
        }

        nextFrame += frameInterval;
        std::this_thread::sleep_until(nextFrame);
        // If we fell behind by more than a frame, resync rather than spiral.
        if (clock::now() > nextFrame + frameInterval) nextFrame = clock::now();
    }

    audio.stop();
    capture.close();
    fprintf(stderr, "[life] stopped after %u frames\n", runner->frameIndex());
    return 0;
}
