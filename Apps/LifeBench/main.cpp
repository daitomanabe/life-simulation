// Apps/LifeBench/main.cpp
// GPU benchmark runner (design doc §4.4 / §18): per-pass GPU time across
// resolutions, resource memory, readback cost. Prints a table and writes
// benchmark.json.

#include "Apps/Common/AppCommon.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Sim/ModuleFactory.h"
#include "LifeCore/Sim/SceneRunner.h"

#include <CLI11/CLI11.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace life;

struct SizeSpec {
    uint32_t w, h;
};

static bool parseSizes(const std::string& s, std::vector<SizeSpec>& out) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        auto x = item.find('x');
        if (x == std::string::npos) {
            uint32_t n = uint32_t(std::stoul(item));
            out.push_back({n, n});
        } else {
            out.push_back({uint32_t(std::stoul(item.substr(0, x))),
                           uint32_t(std::stoul(item.substr(x + 1)))});
        }
    }
    return !out.empty();
}

int main(int argc, char** argv) {
    CLI::App app{"LifeBench — GPU pass timing benchmark"};

    std::string scenePath = "Presets/default.json";
    std::string shaderRoot;
    std::string sizesArg = "512,1024,1920x1080";
    std::string outputPath = "benchmark.json";
    uint32_t frames = 240;
    uint32_t warmup = 30;
    uint32_t substeps = 1;
    bool benchReadback = true;

    app.add_option("--scene", scenePath, "Scene JSON path");
    app.add_option("--shaders", shaderRoot, "Shaders/ directory");
    app.add_option("--sizes", sizesArg, "Comma list: 512,1024,1920x1080,...");
    app.add_option("--frames", frames, "Frames to measure per size");
    app.add_option("--warmup", warmup, "Warmup frames (not measured)");
    app.add_option("--substeps", substeps, "Simulation substeps per frame");
    app.add_option("--output", outputPath, "benchmark JSON output path");
    app.add_flag("!--no-readback", benchReadback, "Skip readback cost measurement");
    CLI11_PARSE(app, argc, argv);

    modules::registerBuiltinModules();

    std::string err;
    auto scene = Scene::loadFile(scenePath, err);
    if (!scene) {
        fprintf(stderr, "[life] %s\n", err.c_str());
        return 1;
    }
    std::vector<SizeSpec> sizes;
    if (!parseSizes(sizesArg, sizes)) {
        fprintf(stderr, "[life] bad --sizes\n");
        return 1;
    }

    nlohmann::json report;
    report["scene"] = scenePath;
    report["frames"] = frames;
    report["substeps"] = substeps;
    report["results"] = nlohmann::json::array();

    for (const auto& size : sizes) {
        Scene s = *scene;
        s.width = size.w;
        s.height = size.h;

        SceneRunnerDesc rd;
        rd.scene = s;
        rd.shaderRoot = app::resolveShaderRoot(shaderRoot, argv[0]);
        rd.substeps = substeps;
        auto runner = SceneRunner::create(rd, err);
        if (!runner) {
            fprintf(stderr, "[life] %ux%u: %s\n", size.w, size.h, err.c_str());
            continue;
        }
        if (report["device"].is_null())
            report["device"] = runner->metal().deviceName();

        AudioFeatureState silent;
        const float dt = 1.0f / 60.0f;

        for (uint32_t i = 0; i < warmup; ++i) runner->step(silent, dt);

        std::map<std::string, double> passSum;
        std::map<std::string, uint32_t> passCount;
        double gpuSum = 0.0, cpuSum = 0.0, readbackSum = 0.0;
        uint32_t readbackFrames = 0;

        for (uint32_t i = 0; i < frames; ++i) {
            // Periodically include a readback frame to measure its cost.
            bool readback = benchReadback && (i % 60 == 59);
            auto t0 = std::chrono::steady_clock::now();
            StepOptions opts;
            opts.readback = readback;
            runner->step(silent, dt, opts);
            auto t1 = std::chrono::steady_clock::now();

            cpuSum += std::chrono::duration<double, std::milli>(t1 - t0).count();
            double frameGpu = runner->graph().lastFrameGPUms();
            if (readback) {
                readbackSum += frameGpu;
                readbackFrames++;
            } else {
                gpuSum += frameGpu;
            }
            for (const auto& t : runner->graph().lastPassTimings()) {
                passSum[t.label] += t.gpuMilliseconds;
                passCount[t.label]++;
            }
        }

        uint32_t plainFrames = frames - readbackFrames;
        double gpuAvg = plainFrames ? gpuSum / plainFrames : 0.0;
        double cpuAvg = cpuSum / frames;
        double readbackAvg = readbackFrames ? readbackSum / readbackFrames : 0.0;

        printf("\n== %ux%u (substeps %u) ==\n", size.w, size.h, substeps);
        printf("  gpu/frame  %7.3f ms   (%.0f fps capacity)\n", gpuAvg,
               gpuAvg > 0 ? 1000.0 / gpuAvg : 0.0);
        printf("  cpu/frame  %7.3f ms   (incl. GPU wait)\n", cpuAvg);
        if (benchReadback)
            printf("  +readback  %7.3f ms   (delta %+.3f)\n", readbackAvg,
                   readbackAvg - gpuAvg);
        printf("  textures   %u (%.1f MB) | buffers %u (%.1f MB)\n",
               runner->resources().liveTextureCount(),
               runner->resources().textureMemoryBytes() / 1048576.0,
               runner->resources().liveBufferCount(),
               runner->resources().bufferMemoryBytes() / 1048576.0);
        printf("  passes:\n");

        nlohmann::json passes = nlohmann::json::array();
        for (const auto& [label, sum] : passSum) {
            double avg = sum / passCount[label];
            printf("    %-28s %7.3f ms\n", label.c_str(), avg);
            passes.push_back({{"pass", label}, {"avgGpuMs", avg}});
        }

        report["results"].push_back(
            {{"width", size.w},
             {"height", size.h},
             {"avgGpuMs", gpuAvg},
             {"avgCpuMs", cpuAvg},
             {"avgReadbackFrameGpuMs", readbackAvg},
             {"fpsCapacity", gpuAvg > 0 ? 1000.0 / gpuAvg : 0.0},
             {"textureMB", runner->resources().textureMemoryBytes() / 1048576.0},
             {"bufferMB", runner->resources().bufferMemoryBytes() / 1048576.0},
             {"passes", passes}});
    }

    std::ofstream out(outputPath);
    out << report.dump(2) << "\n";
    printf("\n[life] wrote %s\n", outputPath.c_str());
    return 0;
}
