// Apps/LifeOfflineRender/main.cpp
// High-resolution deterministic renderer (design doc §4.3 / §17): fixed dt,
// fixed seed, optional recorded audio replay, frame sequence + metadata,
// resumable (existing frames are skipped for writing; simulation still runs
// so state stays exact).

#include "Apps/Common/AppCommon.h"
#include "LifeCore/IO/CaptureReplay.h"
#include "LifeCore/IO/MovieWriter.h"
#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/ResourcePool.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Sim/ModuleFactory.h"
#include "LifeCore/Sim/SceneRunner.h"

#include <CLI11/CLI11.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace life;

namespace {

bool parseMovieCodec(const std::string& s, MovieCodec& out) {
    if (s == "prores422") { out = MovieCodec::ProRes422; return true; }
    if (s == "prores4444") { out = MovieCodec::ProRes4444; return true; }
    if (s == "h264") { out = MovieCodec::H264; return true; }
    if (s == "hevc") { out = MovieCodec::HEVC; return true; }
    return false;
}

// Independent GPU->CPU readback of the render target, feeding only
// MovieWriter::appendFrame(). SceneRunner's own FrameRecorder (used by
// dumpPNG/dumpEXR below) is a private member with no accessor, and this
// phase's change is scoped to MovieWriter + this file (docs/specs/
// phase10_movie_writer.md — SceneRunner/FrameRecorder are left untouched
// so this doesn't collide with parallel work on other modules). So rather
// than reuse FrameRecorder's already-fetched buffer, this mirrors its
// encodeReadback()/fetch() technique locally via SceneRunner's public
// graph()/resources()/outputTexture(). When PNG/EXR readback also runs for
// the same frame this means the render target is blitted to CPU twice;
// for an offline batch tool that's an acceptable trade for not touching
// shared files.
class MovieFrameSource {
public:
    bool fetch(SceneRunner& runner, std::string& outError) {
        ResourcePool& pool = runner.resources();
        CommandGraph& graph = runner.graph();
        TextureHandle tex = runner.outputTexture();

        auto desc = pool.textureDesc(tex);
        if (desc.format != PixelFormat::RGBA16F) {
            outError = "movie readback expects an RGBA16F render target";
            return false;
        }
        size_t needed = size_t(desc.width) * desc.height * 8;
        if (!buffer_.valid() || pool.bufferSize(buffer_) < needed) {
            if (buffer_.valid()) pool.release(buffer_);
            BufferDesc bd;
            bd.size = needed;
            bd.storage = StorageMode::Shared;
            bd.label = "LifeOfflineRender.movieReadback";
            buffer_ = pool.createBuffer(bd);
            if (!buffer_.valid()) {
                outError = "failed to allocate movie readback buffer";
                return false;
            }
        }
        width_ = desc.width;
        height_ = desc.height;

        // The simulation frame that produced `tex` has already been
        // committed and waited on inside SceneRunner::step(); this is a
        // small standalone command buffer just for the blit copy.
        graph.beginFrame(runner.frameIndex());
        graph.copyTextureToBuffer(tex, buffer_);
        graph.endFrame(/*waitUntilCompleted=*/true);

        void* src = pool.bufferContents(buffer_);
        if (!src) {
            outError = "movie readback buffer is not CPU-visible";
            return false;
        }
        pixels_.resize(size_t(width_) * height_ * 4);
        std::memcpy(pixels_.data(), src, pixels_.size() * sizeof(uint16_t));
        return true;
    }

    const uint16_t* data() const { return pixels_.data(); }

private:
    BufferHandle buffer_;
    uint32_t width_ = 0, height_ = 0;
    std::vector<uint16_t> pixels_;
};

} // namespace

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
    std::string movieOut;
    std::string movieCodecStr = "prores422";
    float movieQuality = 0.9f;

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
    app.add_option("--movie", movieOut, "Write a .mov alongside the frame sequence (AVAssetWriter)");
    app.add_option("--codec", movieCodecStr, "prores422 | prores4444 | h264 | hevc (default prores422)");
    app.add_option("--quality", movieQuality, "H264/HEVC quality 0..1 (default 0.9)");
    CLI11_PARSE(app, argc, argv);

    const bool wantMovie = !movieOut.empty();
    if (wantMovie && resume) {
        fprintf(stderr,
                "[life] --movie cannot be combined with --resume (movie output cannot be "
                "resumed/appended)\n");
        return 1;
    }
    MovieCodec movieCodec = MovieCodec::ProRes422;
    if (wantMovie && !parseMovieCodec(movieCodecStr, movieCodec)) {
        fprintf(stderr, "[life] unknown --codec '%s' (expected prores422 | prores4444 | h264 | hevc)\n",
                movieCodecStr.c_str());
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

    MovieWriter movieWriter;
    MovieFrameSource movieSource;
    if (wantMovie) {
        MovieWriterDesc md;
        md.path = movieOut;
        md.width = scene->width;
        md.height = scene->height;
        md.fps = fps;
        md.codec = movieCodec;
        md.quality = movieQuality;
        md.exposure = exposure;
        if (!movieWriter.open(md, err)) {
            fprintf(stderr, "[life] movie: %s\n", err.c_str());
            return 1;
        }
        fprintf(stderr, "[life] movie: %s (%s)\n", movieOut.c_str(), movieCodecStr.c_str());
    }

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
        // movie export needs a readback every frame even on frames where
        // PNG/EXR aren't being (re)written (spec §2).
        bool needReadback = needPNG || needEXR || wantMovie;

        StepOptions opts;
        opts.readback = needReadback;
        runner->step(state, dt, opts);
        gpuMsSum += runner->graph().lastFrameGPUms();

        if (needPNG && !runner->dumpPNG(pngPath, exposure, err))
            fprintf(stderr, "[life] frame %u: %s\n", i, err.c_str());
        if (needEXR && !runner->dumpEXR(exrPath, err))
            fprintf(stderr, "[life] frame %u: %s\n", i, err.c_str());
        if (wantMovie) {
            std::string movieErr;
            if (!movieSource.fetch(*runner, movieErr) ||
                !movieWriter.appendFrame(movieSource.data(), movieErr)) {
                fprintf(stderr, "[life] frame %u movie: %s\n", i, movieErr.c_str());
            }
        }
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

    bool movieOk = true;
    if (wantMovie) {
        meta["movie"] = {{"path", movieOut},
                         {"codec", movieCodecStr},
                         {"quality", movieQuality},
                         {"framesWritten", movieWriter.framesWritten()}};
        if (!movieWriter.finish(err)) {
            fprintf(stderr, "[life] movie: %s\n", err.c_str());
            movieOk = false;
        } else {
            std::error_code sizeEc;
            uintmax_t bytes = fs::file_size(movieOut, sizeEc);
            double mb = sizeEc ? 0.0 : double(bytes) / (1024.0 * 1024.0);
            fprintf(stderr, "[life] movie: %llu frames -> %s (%.1f MB)\n",
                    (unsigned long long)movieWriter.framesWritten(), movieOut.c_str(), mb);
        }
    }

    std::ofstream metaFile(fs::path(outputDir) / "metadata.json");
    metaFile << meta.dump(2) << "\n";

    auto wallEnd = std::chrono::steady_clock::now();
    fprintf(stderr, "[life] done: %u written, %u skipped, wall %.1fs → %s\n", written,
            skipped, std::chrono::duration<double>(wallEnd - wallStart).count(),
            outputDir.c_str());
    return movieOk ? 0 : 1;
}
