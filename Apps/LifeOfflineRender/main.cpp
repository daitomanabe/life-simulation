// Apps/LifeOfflineRender/main.cpp
// High-resolution deterministic renderer (design doc §4.3 / §17): fixed dt,
// fixed seed, optional recorded audio replay, frame sequence + metadata,
// resumable (existing frames are skipped for writing; simulation still runs
// so state stays exact).

#include "Apps/Common/AppCommon.h"
#include "LifeCore/IO/CaptureReplay.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Sim/ModuleFactory.h"
#include "LifeCore/Sim/SceneRunner.h"

#include <CLI11/CLI11.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace life;

int main(int argc, char** argv) {
    CLI::App app{"LifeOfflineRender — deterministic offline renderer"};

    std::string scenePath = "Presets/default.json";
    std::string shaderRoot;
    std::string audioCapture;
    std::string outputDir = "renders/out";
    std::string format = "png"; // png | exr | both
    int width = -1, height = -1;
    double fps = 30.0;
    uint32_t frames = 300;
    uint32_t substeps = 1;
    int64_t seed = -1;
    float exposure = 1.0f;
    bool resume = false;

    app.add_option("--scene", scenePath, "Scene JSON path");
    app.add_option("--shaders", shaderRoot, "Shaders/ directory");
    app.add_option("--width", width, "Override scene width");
    app.add_option("--height", height, "Override scene height");
    app.add_option("--fps", fps, "Simulation frame rate (fixed dt = 1/fps)");
    app.add_option("--frames", frames, "Number of frames to render");
    app.add_option("--substeps", substeps, "Simulation substeps per frame");
    app.add_option("--seed", seed, "Override scene seed");
    app.add_option("--audio", audioCapture, "AudioFeatureState capture (.jsonl) to replay");
    app.add_option("--output", outputDir, "Output directory");
    app.add_option("--format", format, "png | exr | both");
    app.add_option("--exposure", exposure, "PNG exposure");
    app.add_flag("--resume", resume, "Skip frames whose files already exist");
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

    CaptureReader capture;
    bool hasCapture = false;
    if (!audioCapture.empty()) {
        if (!capture.load(audioCapture, err)) {
            fprintf(stderr, "[life] %s\n", err.c_str());
            return 1;
        }
        hasCapture = true;
        fprintf(stderr, "[life] audio capture: %zu frames\n", capture.frameCount());
    }

    if (!app::ensureDirectory(outputDir, err)) {
        fprintf(stderr, "[life] %s\n", err.c_str());
        return 1;
    }

    SceneRunnerDesc rd;
    rd.scene = *scene;
    rd.shaderRoot = app::resolveShaderRoot(shaderRoot, argv[0]);
    rd.substeps = substeps;
    auto runner = SceneRunner::create(rd, err);
    if (!runner) {
        fprintf(stderr, "[life] %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "[life] device: %s | %ux%u | %u frames @ %.1f fps, %u substeps\n",
            runner->metal().deviceName().c_str(), scene->width, scene->height, frames,
            fps, substeps);

    const bool writePNG = format == "png" || format == "both";
    const bool writeEXR = format == "exr" || format == "both";
    const float dt = float(1.0 / fps);

    auto wallStart = std::chrono::steady_clock::now();
    double gpuMsSum = 0.0;
    uint32_t written = 0, skipped = 0;

    for (uint32_t i = 0; i < frames; ++i) {
        AudioFeatureState state;
        if (hasCapture) state = capture.frame(i);
        // Offline clock is authoritative regardless of capture timing.
        state.time = float(i) * dt;
        state.deltaTime = dt;
        state.frameIndex = i;

        std::string pngPath = app::frameFilename(outputDir, "frame", i, "png");
        std::string exrPath = app::frameFilename(outputDir, "frame", i, "exr");
        bool needPNG = writePNG && !(resume && fs::exists(pngPath));
        bool needEXR = writeEXR && !(resume && fs::exists(exrPath));
        bool needReadback = needPNG || needEXR;

        StepOptions opts;
        opts.readback = needReadback;
        runner->step(state, dt, opts);
        gpuMsSum += runner->graph().lastFrameGPUms();

        if (needPNG && !runner->dumpPNG(pngPath, exposure, err))
            fprintf(stderr, "[life] frame %u: %s\n", i, err.c_str());
        if (needEXR && !runner->dumpEXR(exrPath, err))
            fprintf(stderr, "[life] frame %u: %s\n", i, err.c_str());
        needReadback ? written++ : skipped++;

        if (i % 60 == 0 || i + 1 == frames) {
            auto now = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(now - wallStart).count();
            fprintf(stderr, "[life] frame %u/%u (%.1f%%) wall %.1fs gpu avg %.2fms\n",
                    i + 1, frames, 100.0 * (i + 1) / frames, sec,
                    gpuMsSum / (i + 1));
        }
    }

    // Metadata snapshot for reproducibility (design doc §17.2).
    nlohmann::json meta;
    meta["scene"] = scene->raw;
    meta["sceneFile"] = scenePath;
    meta["seed"] = scene->seed;
    meta["resolution"] = {scene->width, scene->height};
    meta["fps"] = fps;
    meta["frames"] = frames;
    meta["substeps"] = substeps;
    meta["format"] = format;
    meta["exposure"] = exposure;
    meta["audioCapture"] = audioCapture;
    meta["device"] = runner->metal().deviceName();
    meta["framesWritten"] = written;
    meta["framesSkipped"] = skipped;
    {
        nlohmann::json passes = nlohmann::json::array();
        for (const auto& t : runner->graph().lastPassTimings())
            passes.push_back({{"pass", t.label}, {"gpuMs", t.gpuMilliseconds}});
        meta["lastFramePassTimings"] = passes;
        meta["avgFrameGPUms"] = gpuMsSum / std::max(1u, frames);
    }
    std::ofstream metaFile(fs::path(outputDir) / "metadata.json");
    metaFile << meta.dump(2) << "\n";

    auto wallEnd = std::chrono::steady_clock::now();
    fprintf(stderr, "[life] done: %u written, %u skipped, wall %.1fs → %s\n", written,
            skipped, std::chrono::duration<double>(wallEnd - wallStart).count(),
            outputDir.c_str());
    return 0;
}
