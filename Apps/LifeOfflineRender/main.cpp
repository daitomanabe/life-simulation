// Apps/LifeOfflineRender/main.cpp
// High-resolution deterministic renderer (design doc §4.3 / §17): fixed dt,
// fixed seed, optional recorded audio replay, frame sequence + metadata,
// resumable (existing frames are skipped for writing; simulation still runs
// so state stays exact).

#include "Apps/Common/AppCommon.h"
#include "LifeCore/IO/CaptureReplay.h"
#include "LifeCore/IO/MovieWriter.h"
#include "LifeCore/Music/MusicTimeline.h"
#include "LifeCore/Metal/CommandGraph.h"
#include "LifeCore/Metal/ResourcePool.h"
#include "LifeCore/Params/Scene.h"
#include "LifeCore/Sim/ModuleFactory.h"
#include "LifeCore/Sim/SceneRunner.h"

#include <CLI11/CLI11.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
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
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    BufferHandle buffer_;
    uint32_t width_ = 0, height_ = 0;
    std::vector<uint16_t> pixels_;
};

// --raw: 最終フレームを 8bit 輝度の生バイト列として吐く。ヘッダも区切りもない、
// width*height バイトが frames 回続くだけ。受け手（Python の VJ 合成器）は
// 解像度を知っているのでそれで足りる。
//
// これは Metal への段階移行の *仮設* 配管である。VJ 側の描画が LifeCore の
// モジュールになった時点で不要になり、削除される。だからこそ 40 行で済ませる。
//
// 重み 0.30/0.55/0.15 は visualize-lyria の vj/gl.py read_luma() と同一。
// mono パレットなら r=g=b なので実際には効かないが、色付きシーンを繋いだ時に
// 両者の輝度が食い違わないようにしておく。
class RawLumaWriter {
public:
    bool open(const std::string& path, std::string& outError) {
        f_ = (path == "-") ? stdout : std::fopen(path.c_str(), "wb");
        if (!f_) {
            outError = "cannot open --raw output: " + path;
            return false;
        }
        ownsFile_ = (f_ != stdout);
        return true;
    }
    bool write(const MovieFrameSource& src, std::string& outError) {
        const size_t n = size_t(src.width()) * src.height();
        line_.resize(n);
        const uint16_t* p = src.data();
        for (size_t i = 0; i < n; ++i) {
            __fp16 r, g, b;
            std::memcpy(&r, &p[i * 4 + 0], 2);
            std::memcpy(&g, &p[i * 4 + 1], 2);
            std::memcpy(&b, &p[i * 4 + 2], 2);
            float y = float(r) * 0.30f + float(g) * 0.55f + float(b) * 0.15f;
            y = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
            line_[i] = uint8_t(y * 255.0f + 0.5f);
        }
        if (std::fwrite(line_.data(), 1, n, f_) != n) {
            outError = "short write on --raw output (reader closed the pipe?)";
            return false;
        }
        return true;
    }
    ~RawLumaWriter() {
        if (f_ && ownsFile_) std::fclose(f_);
        else if (f_) std::fflush(f_);
    }

private:
    std::FILE* f_ = nullptr;
    bool ownsFile_ = false;
    std::vector<uint8_t> line_;
};

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"LifeOfflineRender — deterministic offline renderer"};

    std::string scenePath = "Presets/default.json";
    std::string shaderRoot;
    std::string audioCapture;
    std::string musicTimeline;
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
    std::string rawOut;
    std::string dumpParams;

    app.add_option("--scene", scenePath, "Scene JSON path");
    app.add_option("--shaders", shaderRoot, "Shaders/ directory");
    app.add_option("--width", width, "Override scene width");
    app.add_option("--height", height, "Override scene height");
    app.add_option("--fps", fps, "Simulation frame rate (fixed dt = 1/fps)");
    app.add_option("--frames", frames, "Number of frames to render");
    app.add_option("--substeps", substeps, "Simulation substeps per frame");
    app.add_option("--seed", seed, "Override scene seed");
    app.add_option("--audio", audioCapture, "AudioFeatureState capture (.jsonl) to replay");
    app.add_option("--music", musicTimeline,
                   "remix-beats events JSON — sections / events / 60fps curves");
    app.add_option("--output", outputDir, "Output directory");
    app.add_option("--format", format, "png | exr | both");
    app.add_option("--exposure", exposure, "PNG exposure");
    app.add_flag("--resume", resume, "Skip frames whose files already exist");
    app.add_option("--movie", movieOut, "Write a .mov alongside the frame sequence (AVAssetWriter)");
    app.add_option("--codec", movieCodecStr, "prores422 | prores4444 | h264 | hevc (default prores422)");
    app.add_option("--quality", movieQuality, "H264/HEVC quality 0..1 (default 0.9)");
    app.add_option("--raw", rawOut,
                   "Stream frames as headerless 8-bit luma (w*h bytes each). "
                   "'-' writes to stdout. Logs always go to stderr.");
    app.add_option("--dump-params", dumpParams,
                   "Comma-separated ParameterBus keys (e.g. lenia0.growthMu,"
                   "slime0.moveSpeed). Writes <output>/params.csv, one row per "
                   "frame. The only honest way to answer \"is it reacting?\".");
    CLI11_PARSE(app, argc, argv);
    const bool wantRaw = !rawOut.empty();

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

    MusicTimeline music;
    if (!musicTimeline.empty()) {
        if (!music.load(musicTimeline, err)) {
            fprintf(stderr, "[life] %s\n", err.c_str());
            return 1;
        }
        auto types = music.eventTypes();
        fprintf(stderr,
                "[life] music: %s  %.1f bpm  %u frames  %zu sections  "
                "%zu events (%zu types)\n",
                music.trackName().c_str(), music.bpm(), music.frameCount(),
                music.sections().size(), music.events().size(), types.size());
        // シーンが参照しているイベント型がこの曲に存在しないなら、黙って 0 を
        // 返し続けるより先に言う。曲ごとにイベント構成が違うので事故りやすい。
        for (const auto& m : scene->audioMappings) {
            if (m.source.rfind("event:", 0) != 0) continue;
            std::string t = m.source.substr(6);
            if (auto dot = t.find('.'); dot != std::string::npos) t = t.substr(0, dot);
            if (std::find(types.begin(), types.end(), t) == types.end())
                fprintf(stderr,
                        "[life] warning: mapping source '%s' — this track has no "
                        "event of type '%s' (mapping will read 0)\n",
                        m.source.c_str(), t.c_str());
        }
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

    std::vector<std::string> dumpKeys;
    std::ofstream dumpFile;
    if (!dumpParams.empty()) {
        size_t p = 0;
        while (p <= dumpParams.size()) {
            size_t c = dumpParams.find(',', p);
            if (c == std::string::npos) c = dumpParams.size();
            if (c > p) dumpKeys.push_back(dumpParams.substr(p, c - p));
            p = c + 1;
        }
        dumpFile.open(outputDir + "/params.csv", std::ios::trunc);
        if (!dumpFile) {
            fprintf(stderr, "[life] cannot write %s/params.csv\n", outputDir.c_str());
            return 1;
        }
        dumpFile << "frame";
        for (const auto& k : dumpKeys) dumpFile << "," << k;
        dumpFile << "\n";
        fprintf(stderr, "[life] dumping %zu params -> %s/params.csv\n", dumpKeys.size(),
                outputDir.c_str());
    }

    RawLumaWriter rawWriter;
    if (wantRaw) {
        if (!rawWriter.open(rawOut, err)) {
            fprintf(stderr, "[life] %s\n", err.c_str());
            return 1;
        }
        fprintf(stderr, "[life] raw luma: %s (%ux%u, %u bytes/frame)\n", rawOut.c_str(),
                scene->width, scene->height, scene->width * scene->height);
    }

    const bool writePNG = format == "png" || format == "both";
    const bool writeEXR = format == "exr" || format == "both";
    const float dt = float(1.0 / fps);

    auto wallStart = std::chrono::steady_clock::now();
    double gpuMsSum = 0.0;
    uint32_t written = 0, skipped = 0;

    for (uint32_t i = 0; i < frames; ++i) {
        MusicFeatureState ms;
        if (hasCapture) ms.audio = capture.frame(i);
        // Offline clock is authoritative regardless of capture timing.
        ms.audio.time = float(i) * dt;
        ms.audio.deltaTime = dt;
        ms.audio.frameIndex = i;
        ms.frame = i;
        if (music.loaded()) music.sample(i, ms);

        std::string pngPath = app::frameFilename(outputDir, "frame", i, "png");
        std::string exrPath = app::frameFilename(outputDir, "frame", i, "exr");
        bool needPNG = writePNG && !(resume && fs::exists(pngPath));
        bool needEXR = writeEXR && !(resume && fs::exists(exrPath));
        // movie export needs a readback every frame even on frames where
        // PNG/EXR aren't being (re)written (spec §2).
        bool needReadback = needPNG || needEXR || wantMovie || wantRaw;

        StepOptions opts;
        opts.readback = needReadback;
        runner->step(ms, dt, opts);
        gpuMsSum += runner->graph().lastFrameGPUms();

        if (dumpFile.is_open()) {
            dumpFile << i;
            for (const auto& k : dumpKeys)
                dumpFile << "," << runner->params().value(k, 0.0f);
            dumpFile << "\n";
        }

        if (needPNG && !runner->dumpPNG(pngPath, exposure, err))
            fprintf(stderr, "[life] frame %u: %s\n", i, err.c_str());
        if (needEXR && !runner->dumpEXR(exrPath, err))
            fprintf(stderr, "[life] frame %u: %s\n", i, err.c_str());
        if (wantMovie || wantRaw) {
            // 1 フレームにつき readback は 1 回だけ。--movie と --raw を同時に
            // 指定しても GPU→CPU コピーは重複しない。
            std::string ferr;
            if (!movieSource.fetch(*runner, ferr)) {
                fprintf(stderr, "[life] frame %u readback: %s\n", i, ferr.c_str());
            } else {
                if (wantMovie && !movieWriter.appendFrame(movieSource.data(), ferr))
                    fprintf(stderr, "[life] frame %u movie: %s\n", i, ferr.c_str());
                if (wantRaw && !rawWriter.write(movieSource, ferr)) {
                    // パイプの読み手が閉じたら、静かに終わるのが正しい。
                    fprintf(stderr, "[life] frame %u raw: %s\n", i, ferr.c_str());
                    return 1;
                }
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
