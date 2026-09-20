// Apps/LifeRealtime/main.cpp
// Headless-by-default VJ runner (design doc §4.2 / §16): OSC in, 60 fps
// pacing, status line once per second, optional periodic frame dump and
// audio capture recording. Live output (--preview window, --syphon server)
// is opt-in via OutputSink (phase 6, design doc §3.8) and adds zero
// overhead to the default headless path when not requested.

#include "Apps/Common/AppCommon.h"
#include "LifeCore/Audio/AudioInput.h"
#include "LifeCore/IO/CaptureReplay.h"
#include "LifeCore/Output/WindowPreviewSink.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Sim/ModuleFactory.h"
#include "LifeCore/Sim/SceneRunner.h"

#if defined(LIFE_WITH_SYPHON)
#include "LifeCore/Output/SyphonSink.h"
#include "LifeCore/Output/FaceSyphonSink.h"
#endif

#include <CLI11/CLI11.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace life;

static std::atomic<bool> gRunning{true};

static void onSignal(int) { gRunning.store(false); }

namespace {

// Gallery-survivability helpers (design doc: unattended install). Kept
// local to this app — same convention as LifeOfflineRender's RawLumaWriter/
// MovieFrameSource local helper classes — since only LifeRealtime needs
// them.

// Write-to-temp-then-rename: a reader (external watchdog, an operator's
// `cp`) can never observe a half-written file, only the old content or the
// new content.
bool writeFileAtomic(const std::string& path, const std::string& contents) {
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
    std::fclose(f);
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// unixTime -> "YYYY-MM-DD" in UTC, so --health-log rotation doesn't depend
// on the machine's TZ setting.
std::string utcDateString(time_t t) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return buf;
}

// Nearest-rank percentile (p in [0,1]); sorts `v` in place (called on a
// short-lived per-window copy, never the live sample buffer).
double percentileOf(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = size_t(p * double(v.size() - 1) + 0.5);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

// Appends one CSV row per --health-log flush (every 10s), rotating the file
// by UTC day (`PATH.YYYY-MM-DD`) so an unattended multi-day run can never
// grow one file without bound. Header written once per file — checked via
// the freshly-opened file's on-disk size, so re-running on the same day
// appends to (rather than re-headers) today's file.
class HealthLog {
public:
    void write(const std::string& basePath, time_t unixTime, uint32_t frame, double fpsP50,
               double fpsP99, double gpuP50, double gpuP99, double oscPps) {
        std::string day = utcDateString(unixTime);
        if (day != day_ || !file_.is_open()) {
            if (file_.is_open()) file_.close();
            day_ = day;
            std::string path = basePath + "." + day;
            bool isNew = !std::filesystem::exists(path);
            file_.open(path, std::ios::app);
            if (!file_) return;
            if (isNew) file_ << "unixTime,frame,fps_p50,fps_p99,gpu_ms_p50,gpu_ms_p99,osc_pps\n";
        }
        file_ << unixTime << "," << frame << "," << fpsP50 << "," << fpsP99 << "," << gpuP50 << ","
              << gpuP99 << "," << oscPps << "\n";
        file_.flush();
    }

private:
    std::ofstream file_;
    std::string day_;
};

} // namespace

// "6816x864" -> (6816, 864). Same "WxH" convention as LifeBench's --sizes.
static bool parseSize(const std::string& s, uint32_t& outW, uint32_t& outH) {
    auto x = s.find('x');
    if (x == std::string::npos) return false;
    outW = uint32_t(std::stoul(s.substr(0, x)));
    outH = uint32_t(std::stoul(s.substr(x + 1)));
    return outW > 0 && outH > 0;
}

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
    bool preview = false;
    float previewScale = 0.5f;
    std::string syphonName;
    uint32_t syphonFaces = 0; // 0 = off
    std::string faceSizeArg = "6816x864";
    std::string loadStateDir;
    std::string dumpStateDir;
    double dumpStateEvery = 0.0; // seconds, 0 = off
    std::string heartbeatPath;
    double stallMs = 50.0;
    std::string healthLogPath;

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
    app.add_flag("--preview", preview, "Show a live preview window");
    app.add_option("--preview-scale", previewScale,
                   "Preview window content size relative to scene resolution (default 0.5)");
    app.add_option("--syphon", syphonName,
                   "Publish frames as a Syphon server with this name (empty = disabled)");
    app.add_option("--syphon-faces", syphonFaces,
                   "Publish N per-face Syphon servers ('<syphon> west/north/east' for N=3, "
                   "'<syphon> 1'.. for other N; 0 = disabled). Independent of --syphon.");
    app.add_option("--face-size", faceSizeArg,
                   "Per-face Syphon output size as WxH (default 6816x864)");
    app.add_option("--load-state", loadStateDir,
                   "Warm-start from a --dump-state snapshot directory (LifeOfflineRender or "
                   "this app's own --dump-state). Any failure (missing/corrupt/mismatched "
                   "snapshot) logs a warning and falls back to a normal cold start — a "
                   "gallery must never fail to open because a snapshot went bad.");
    app.add_option("--dump-state", dumpStateDir,
                   "Base directory for periodic state snapshots (see --dump-state-every). "
                   "Written to <dir>/latest, with the previous snapshot kept at <dir>/prev "
                   "as a fallback.");
    app.add_option("--dump-state-every", dumpStateEvery,
                   "Write a --dump-state snapshot every N seconds (0 = off, default). Writes "
                   "to a temp dir then renames into place, so a snapshot is never observed "
                   "half-written.");
    app.add_option("--heartbeat", heartbeatPath,
                   "Write a liveness JSON file here every second (write-temp-then-rename), for "
                   "an external watchdog (see ops/watchdog.sh). Also enables self-exit on a "
                   "GPU stall or Metal command buffer error (off otherwise, so interactive use "
                   "is unaffected).");
    app.add_option("--stall-ms", stallMs,
                   "With --heartbeat: self-exit(1) if the last ~300 frames (~5s) all took "
                   "longer than this many GPU ms (default 50)");
    app.add_option("--health-log", healthLogPath,
                   "Append one CSV row every 10s (fps/GPU-ms p50/p99, OSC packet rate) to "
                   "PATH.YYYY-MM-DD, rotating by UTC day.");
    CLI11_PARSE(app, argc, argv);

    uint32_t faceOutW = 0, faceOutH = 0;
    if (syphonFaces > 0 && !parseSize(faceSizeArg, faceOutW, faceOutH)) {
        fprintf(stderr, "[life] --face-size expects WxH, got '%s'\n", faceSizeArg.c_str());
        return 1;
    }

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

    // Warm start, after setup()+reset() and before the first step() — any
    // failure (missing dir, corrupt/truncated .npy, scene/resolution/module
    // mismatch) is a warning, not a fatal error: a gallery installation must
    // never fail to open because a snapshot on disk went bad.
    if (!loadStateDir.empty()) {
        std::string lerr;
        if (!runner->loadState(loadStateDir, scenePath, lerr))
            fprintf(stderr, "[life] --load-state: %s — starting cold\n", lerr.c_str());
    }

    if (preview) {
        std::string sceneName = std::filesystem::path(scenePath).stem().string();
        runner->addOutputSink(std::make_unique<WindowPreviewSink>(sceneName, previewScale));
    }
    if (!syphonName.empty()) {
#if defined(LIFE_WITH_SYPHON)
        runner->addOutputSink(std::make_unique<SyphonSink>(syphonName));
#else
        fprintf(stderr,
                "[life] --syphon '%s' requested but this build has LIFE_WITH_SYPHON=OFF\n",
                syphonName.c_str());
#endif
    }
    if (syphonFaces > 0) {
        // Reuses --syphon's name as the base ("<syphon> west" etc) when given
        // (independent of --syphon otherwise); with no --syphon, servers are
        // named bare "west"/"north"/"east".
#if defined(LIFE_WITH_SYPHON)
        runner->addOutputSink(
            std::make_unique<FaceSyphonSink>(syphonName, syphonFaces, faceOutW, faceOutH));
#else
        fprintf(stderr,
                "[life] --syphon-faces %u requested but this build has LIFE_WITH_SYPHON=OFF\n",
                syphonFaces);
#endif
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
    auto prevLoopTop = start;
    uint32_t framesSinceStatus = 0;
    double gpuMsSinceStatus = 0.0;
    uint64_t lastPackets = 0;

    // Liveness self-exit (§2) — only active with --heartbeat, so plain
    // interactive use behaves exactly as before.
    uint32_t consecutiveSlowFrames = 0;
    constexpr uint32_t kStallFrameThreshold = 300; // ~5s at 60fps

    // --health-log (§3): per-frame samples collected over a rolling ~10s
    // window, flushed to one CSV row and cleared.
    std::vector<double> healthFpsSamples, healthGpuSamples;
    double lastHealthFlush = 0.0;
    uint64_t healthPacketsWindowStart = audio.oscStats().packetsReceived;
    HealthLog healthLog;

    // --dump-state-every (§1): periodic snapshot, published atomically.
    double lastDumpStateFlush = 0.0;

    while (gRunning.load()) {
        auto now = clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (duration > 0.0 && elapsed >= duration) break;

        double frameSec = std::chrono::duration<double>(now - prevLoopTop).count();
        double instFps = frameSec > 0.0 ? 1.0 / frameSec : 0.0;
        prevLoopTop = now;

        uint32_t frame = runner->frameIndex();
        const AudioFeatureState& state = audio.update(dt, float(elapsed), frame);
        if (capture.isOpen()) capture.writeFrame(state);

        bool dump = dumpEvery > 0 && frame % dumpEvery == 0;
        StepOptions opts;
        opts.readback = dump;
        runner->step(state, dt, opts);
        double gpuMs = runner->graph().lastFrameGPUms();
        gpuMsSinceStatus += gpuMs;
        framesSinceStatus++;

        if (dump) {
            std::string path = app::frameFilename(dumpDir, "preview", frame, "png");
            if (!runner->dumpPNG(path, exposure, err))
                fprintf(stderr, "[life] dump failed: %s\n", err.c_str());
        }

        if (!healthLogPath.empty()) {
            healthFpsSamples.push_back(instFps);
            healthGpuSamples.push_back(gpuMs);
        }

        // Self-exit: a Metal error, or ~5s of frames all slower than
        // --stall-ms, means the GPU is hung/thrashing and staying alive
        // would just show a frozen or stuttering wall — better to exit(1)
        // and let a supervisor (see ops/*.plist) restart cleanly.
        if (!heartbeatPath.empty()) {
            if (gpuMs > stallMs) consecutiveSlowFrames++; else consecutiveSlowFrames = 0;
            if (consecutiveSlowFrames >= kStallFrameThreshold) {
                fprintf(stderr,
                        "[life] self-exit: %u consecutive frames exceeded --stall-ms %.1f "
                        "(last gpu %.2fms) — exiting for supervisor restart\n",
                        consecutiveSlowFrames, stallMs, gpuMs);
                return 1;
            }
            if (runner->graph().lastFrameHadError()) {
                fprintf(stderr,
                        "[life] self-exit: Metal command buffer reported an error — exiting "
                        "for supervisor restart\n");
                return 1;
            }
        }

        if (dumpStateEvery > 0.0 && !dumpStateDir.empty() &&
            elapsed - lastDumpStateFlush >= dumpStateEvery) {
            lastDumpStateFlush = elapsed;
            std::string tmpDir = dumpStateDir + "/.tmp-latest";
            std::error_code ec;
            std::filesystem::remove_all(tmpDir, ec);
            std::string derr;
            if (!runner->dumpState(tmpDir, fps, scenePath, derr)) {
                fprintf(stderr, "[life] dump-state: %s\n", derr.c_str());
                std::filesystem::remove_all(tmpDir, ec);
            } else {
                std::string latestPath = dumpStateDir + "/latest";
                std::string prevPath = dumpStateDir + "/prev";
                std::filesystem::remove_all(prevPath, ec);
                std::error_code renEc;
                if (std::filesystem::exists(latestPath))
                    std::filesystem::rename(latestPath, prevPath, renEc);
                std::filesystem::rename(tmpDir, latestPath, renEc);
                if (renEc) {
                    fprintf(stderr, "[life] dump-state: publish failed: %s\n",
                            renEc.message().c_str());
                    std::filesystem::remove_all(tmpDir, ec);
                }
            }
        }

        if (!healthLogPath.empty() && elapsed - lastHealthFlush >= 10.0) {
            double windowSec = lastHealthFlush > 0.0 ? elapsed - lastHealthFlush : elapsed;
            uint64_t packetsNow = audio.oscStats().packetsReceived;
            double oscPps =
                windowSec > 0.0 ? double(packetsNow - healthPacketsWindowStart) / windowSec : 0.0;
            healthLog.write(healthLogPath, std::time(nullptr), frame,
                            percentileOf(healthFpsSamples, 0.50), percentileOf(healthFpsSamples, 0.99),
                            percentileOf(healthGpuSamples, 0.50), percentileOf(healthGpuSamples, 0.99),
                            oscPps);
            healthFpsSamples.clear();
            healthGpuSamples.clear();
            healthPacketsWindowStart = packetsNow;
            lastHealthFlush = elapsed;
        }

        // Status once per second (fps, GPU load, OSC traffic, passes).
        now = clock::now();
        if (now - lastStatus >= std::chrono::seconds(1)) {
            double statusSec = std::chrono::duration<double>(now - lastStatus).count();
            auto stats = audio.oscStats();
            double fpsRecent = framesSinceStatus / statusSec;
            double gpuMsRecent = gpuMsSinceStatus / std::max(1u, framesSinceStatus);
            uint64_t oscRecent = stats.packetsReceived - lastPackets;
            fprintf(stderr,
                    "[life] fps %5.1f | gpu %5.2fms | passes %2u | osc %4llu pkt/s | "
                    "kick %.2f snare %.2f hihat %.2f beat %.2f rms %.2f\n",
                    fpsRecent, gpuMsRecent, runner->graph().lastFramePassCount(),
                    (unsigned long long)oscRecent, state.kick, state.snare, state.hihat,
                    state.beat, state.rms);

            if (!heartbeatPath.empty()) {
                char buf[256];
                snprintf(buf, sizeof(buf),
                        "{\"pid\":%d,\"frame\":%u,\"simTime\":%.3f,\"fpsRecent\":%.2f,"
                        "\"gpuMsRecent\":%.3f,\"oscPacketsRecent\":%llu,\"unixTime\":%lld}\n",
                        int(getpid()), frame, frame * dt, fpsRecent, gpuMsRecent,
                        (unsigned long long)oscRecent, (long long)std::time(nullptr));
                if (!writeFileAtomic(heartbeatPath, buf))
                    fprintf(stderr, "[life] heartbeat: failed to write %s\n",
                            heartbeatPath.c_str());
            }

            lastPackets = stats.packetsReceived;
            lastStatus = now;
            framesSinceStatus = 0;
            gpuMsSinceStatus = 0.0;
        }

        nextFrame += frameInterval;
        std::this_thread::sleep_until(nextFrame);
        // If we fell behind by more than a frame, resync rather than spiral.
        if (clock::now() > nextFrame + frameInterval) nextFrame = clock::now();

        bool quitRequested = false;
        runner->pumpOutputs(quitRequested);
        if (quitRequested) break;
    }

    audio.stop();
    capture.close();
    fprintf(stderr, "[life] stopped after %u frames\n", runner->frameIndex());
    return 0;
}
