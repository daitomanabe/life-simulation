// Modules/FieldModules/Lenia.cpp
#include "Modules/FieldModules/Lenia.h"

#include "LifeCore/Math/Random.h"
#include "LifeCore/Sim/SharedTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace life {

namespace {

// Mirrors FFTParams in Shaders/Common/FFT.metal exactly (phase9 §2).
struct FFTParams {
    uint32_t width, height, N, Ns;
    int32_t dir;
    uint32_t normalize;
};
static_assert(sizeof(FFTParams) == 6 * 4,
              "FFTParams layout must stay scalar-packed to match FFT.metal");

// FFTParams with only width/height set — enough for the bounds check inside
// realToComplex/complexToReal/complexMulScale/clearR32F, which don't touch
// N/Ns/dir/normalize.
FFTParams fftBounds(uint32_t w, uint32_t h) {
    FFTParams fp{};
    fp.width = w;
    fp.height = h;
    return fp;
}

uint32_t log2u(uint32_t v) {
    uint32_t r = 0;
    while (v > 1u) {
        v >>= 1;
        ++r;
    }
    return r;
}

bool isPow2(uint32_t v) { return v != 0 && (v & (v - 1u)) == 0; }

// Drives one full 2D FFT (all log2(simW) X-stages, then all log2(simH)
// Y-stages — order between the two axes doesn't matter, they're
// independent separable 1D transforms) over the pingA/pingB RG32F scratch
// pair. pingA must already hold the input complex data on entry. Returns
// whichever of pingA/pingB ends up holding the result (stage count parity
// depends on simW/simH, so callers must not assume it's always pingA).
// Phase9 spec §2: "C++ 側は encodeFFT2D(...) のようなヘルパーを Lenia.cpp
// 内 static でよい".
TextureHandle encodeFFT2D(CommandGraph& graph, const std::string& prefix, TextureHandle pingA,
                          TextureHandle pingB, uint32_t simW, uint32_t simH, int dir) {
    TextureHandle cur = pingA, nxt = pingB;
    uint32_t stagesX = log2u(simW);
    for (uint32_t s = 0; s < stagesX; ++s) {
        FFTParams fp{simW, simH, simW, 1u << s, dir,
                    uint32_t(dir < 0 && s + 1 == stagesX ? 1 : 0)};
        graph.pass(prefix + ".x" + std::to_string(s))
            .pipeline("fftStageX")
            .read(0, cur)
            .write(1, nxt)
            .uniforms(0, fp)
            .dispatch2D(simW / 2, simH);
        std::swap(cur, nxt);
    }
    uint32_t stagesY = log2u(simH);
    for (uint32_t s = 0; s < stagesY; ++s) {
        FFTParams fp{simW, simH, simH, 1u << s, dir,
                    uint32_t(dir < 0 && s + 1 == stagesY ? 1 : 0)};
        graph.pass(prefix + ".y" + std::to_string(s))
            .pipeline("fftStageY")
            .read(0, cur)
            .write(1, nxt)
            .uniforms(0, fp)
            .dispatch2D(simW, simH / 2);
        std::swap(cur, nxt);
    }
    return cur;
}

// Generalized ring-kernel shape (phase9 §4, "既存 buildKernelTexture を
// 一般化"): B = betas.size() concentric shells, u = (r/radiusK)*B,
// K(r) = betas[floor(u)] * bell(u - floor(u), 0.5, kernelShellSigma*B),
// zero outside r in (0, radiusK]. Written directly into a simW*simH image
// with the kernel centered at index (0,0), toroidally wrapped ("fftshift"
// layout) — the placement circular convolution via FFT multiplication
// needs. Normalized so the image sums to 1, matching buildKernelTexture's
// existing Σ=1 convention.
void buildRingKernelImage(uint32_t simW, uint32_t simH, float radiusK, float kernelShellSigma,
                          const std::vector<float>& betas, std::vector<float>& outImage) {
    outImage.assign(size_t(simW) * simH, 0.0f);
    if (betas.empty() || radiusK <= 0.0f) return;
    const uint32_t B = uint32_t(betas.size());
    const float Bf = float(B);

    double sum = 0.0;
    for (uint32_t y = 0; y < simH; ++y) {
        int dy = int(y);
        if (dy > int(simH / 2)) dy -= int(simH);
        for (uint32_t x = 0; x < simW; ++x) {
            int dx = int(x);
            if (dx > int(simW / 2)) dx -= int(simW);
            float r = std::sqrt(float(dx) * float(dx) + float(dy) * float(dy));
            float w = 0.0f;
            if (r > 0.0f && r <= radiusK) {
                float u = (r / radiusK) * Bf;
                uint32_t i = std::min<uint32_t>(uint32_t(u), B - 1);
                float t = (u - float(i) - 0.5f) / (kernelShellSigma * Bf);
                w = betas[i] * std::exp(-0.5f * t * t);
            }
            outImage[size_t(y) * simW + x] = w;
            sum += w;
        }
    }
    if (sum > 0.0) {
        float inv = float(1.0 / sum);
        for (auto& w : outImage) w *= inv;
    }
}

// ---- phase11 (docs/specs/phase11_organisms.md): organism JSON loading ----

// Decoded Presets/organisms/*.json (produced by tools/import_lenia_organism.py
// from the official Chakazul/Lenia catalogue — see that script's header for
// the RLE decoder's exact provenance). betas/kn/gn are retained for
// provenance/fidelity but are NOT fed into the simulation: our engine has
// only ever had one kernel/growth family (the existing truncated-Gaussian
// bump used by buildKernelTexture()/leniaStep), so "useOrganismParams"
// deliberately only overrides the parametric quantities the spec names
// (radius, growthMu, growthSigma, dt) — never the kernel shape itself.
struct OrganismData {
    float R = 13.0f;
    float T = 10.0f;
    float mu = 0.15f;
    float sigma = 0.015f;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> cells; // row-major, width*height, values in [0,1]
};

// Loads + validates an organism JSON file (phase11 spec: "organism JSON の
// ロード/検証（cells 行長の一致、値域 [0,1]）。失敗時は stderr エラー +
// noise init にフォールバック"). Returns false + outError on any problem;
// caller falls back to noise init and leaves organism params unmodified.
bool loadOrganismJSON(const std::string& path, OrganismData& out, std::string& outError) {
    std::ifstream f(path);
    if (!f) {
        outError = "cannot open organism file: " + path;
        return false;
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        outError = std::string("organism JSON parse error: ") + e.what();
        return false;
    }

    if (!j.contains("cells") || !j["cells"].is_array() || j["cells"].empty()) {
        outError = "organism JSON missing non-empty \"cells\" array";
        return false;
    }
    const auto& cellsJ = j["cells"];
    const size_t rows = cellsJ.size();
    if (!cellsJ[0].is_array() || cellsJ[0].empty()) {
        outError = "organism JSON \"cells\" rows must be non-empty arrays";
        return false;
    }
    const size_t cols = cellsJ[0].size();

    std::vector<float> cells;
    cells.reserve(rows * cols);
    for (size_t r = 0; r < rows; ++r) {
        if (!cellsJ[r].is_array() || cellsJ[r].size() != cols) {
            outError = "organism JSON \"cells\" row " + std::to_string(r) +
                       " length mismatch (expected " + std::to_string(cols) + " columns)";
            return false;
        }
        for (size_t c = 0; c < cols; ++c) {
            float v = cellsJ[r][c].get<float>();
            if (v < 0.0f || v > 1.0f) {
                outError = "organism JSON cell value out of [0,1] at row " +
                           std::to_string(r) + " col " + std::to_string(c);
                return false;
            }
            cells.push_back(v);
        }
    }

    out.R = j.value("R", 13.0f);
    out.T = j.value("T", 10.0f);
    out.mu = j.value("mu", 0.15f);
    out.sigma = j.value("sigma", 0.015f);
    out.width = uint32_t(cols);
    out.height = uint32_t(rows);
    out.cells = std::move(cells);
    return true;
}

} // namespace

void LeniaModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    // ---- phase9 §1/§3/§4: convMode, sim-resolution split, kernels[] ----
    std::string convModeStr = params_.value("convMode", std::string("direct"));
    useFFT_ = (convModeStr == "fft");

    uint32_t simW = params_.value("simWidth", 0u);
    uint32_t simH = params_.value("simHeight", 0u);
    simWidth_ = simW ? simW : width_;
    simHeight_ = simH ? simH : height_;

    bool hasKernelsParam = params_.contains("kernels") && params_["kernels"].is_array() &&
                           !params_["kernels"].empty();
    if (hasKernelsParam) {
        for (const auto& kj : params_["kernels"]) {
            if (kernels_.size() >= 4) {
                fprintf(stderr,
                       "[life] %s: \"kernels\" has more than 4 entries; truncating to 4\n",
                       instanceName_.c_str());
                break;
            }
            KernelDef kd;
            kd.radiusScale = kj.value("radiusScale", 1.0f);
            kd.mu = kj.value("mu", 0.15f);
            kd.sigma = kj.value("sigma", 0.017f);
            kd.weight = kj.value("weight", 1.0f);
            kd.betas.clear();
            if (kj.contains("betas") && kj["betas"].is_array()) {
                for (const auto& b : kj["betas"]) kd.betas.push_back(b.get<float>());
            }
            if (kd.betas.empty()) kd.betas.push_back(1.0f);
            kernels_.push_back(std::move(kd));
        }
    }

    if (useFFT_ && (!isPow2(simWidth_) || !isPow2(simHeight_))) {
        fprintf(stderr,
               "[life] %s: convMode=\"fft\" requires simWidth/simHeight to be powers of "
               "two (got %ux%u); falling back to convMode=\"direct\"\n",
               instanceName_.c_str(), simWidth_, simHeight_);
        useFFT_ = false;
    }

    // "kernels" without convMode=fft: setup error (spec §4) -> keep running in
    // direct mode using only kernels[0] (its mu/sigma become the growthMu/
    // growthSigma *fallback default*; an explicit scene growthMu/growthSigma
    // still wins, see encode()). kernels_ itself is only ever consulted when
    // useFFT_, so it is cleared below either way.
    float radiusScaleOverride = 1.0f;
    if (hasKernelsParam && !useFFT_) {
        fprintf(stderr,
               "[life] %s: \"kernels\" requires convMode=\"fft\"; continuing in direct "
               "mode using kernels[0] only\n",
               instanceName_.c_str());
        const KernelDef& k0 = kernels_[0];
        radiusScaleOverride = k0.radiusScale;
        fallbackGrowthMu_ = k0.mu;
        fallbackGrowthSigma_ = k0.sigma;
    }
    if (!useFFT_) kernels_.clear();

    // ---- phase11 §"Lenia モジュール拡張": organism JSON + stamp init ----
    std::string initModeStr = params_.value("initMode", std::string("noise"));
    bool wantStampInit = (initModeStr == "stamp");
    std::string organismPath = params_.value("organism", std::string());
    bool wantOrganismParams = params_.value("useOrganismParams", true);
    stampCount_ = std::min<uint32_t>(32u, params_.value("stampCount", 6u));
    stampRotate_ = params_.value("stampRotate", true);

    // Gated on organismPath being set: useOrganismParams defaults to true
    // library-wide, so without this guard every existing "kernels"-array
    // preset (e.g. lenia_multikernel.json, which never mentions "organism"
    // at all) would spuriously trip this "conflict" on its unrelated
    // default value. There's nothing to actually conflict with unless the
    // scene opted into organism features by setting "organism".
    if (!organismPath.empty() && wantOrganismParams && hasKernelsParam) {
        fprintf(stderr,
               "[life] %s: \"useOrganismParams\" cannot be combined with \"kernels\"; "
               "ignoring organism parameter overrides\n",
               instanceName_.c_str());
        wantOrganismParams = false;
    }
    if (wantStampInit && organismPath.empty()) {
        fprintf(stderr,
               "[life] %s: initMode=\"stamp\" requires an \"organism\" path; falling back "
               "to noise init\n",
               instanceName_.c_str());
        wantStampInit = false;
    }

    uint32_t organismRadiusDefault = 13u;
    OrganismData organism;
    if (!organismPath.empty()) {
        std::string loadErr;
        if (loadOrganismJSON(organismPath, organism, loadErr)) {
            organismLoaded_ = true;
        } else {
            fprintf(stderr,
                   "[life] %s: failed to load organism \"%s\": %s; falling back to noise "
                   "init\n",
                   instanceName_.c_str(), organismPath.c_str(), loadErr.c_str());
            wantStampInit = false; // no cells to stamp
        }
    }

    if (organismLoaded_ && wantOrganismParams) {
        organismRadiusDefault = uint32_t(std::round(organism.R));
        fallbackGrowthMu_ = organism.mu;
        fallbackGrowthSigma_ = organism.sigma;
        if (organism.T > 1e-6f) fallbackDt_ = 1.0f / organism.T;
    }
    useStampInit_ = wantStampInit && organismLoaded_;

    Field2DDesc fd;
    fd.width = simWidth_;
    fd.height = simHeight_;
    fd.format = PixelFormat::R32F;
    fd.pingPong = true;
    fd.boundary = BoundaryMode::Wrap;
    fd.label = instanceName_ + ".state";
    field_.create(*ctx.resources, fd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    gpuParams_.radius =
        uint32_t(std::round(params_.value("radius", organismRadiusDefault) * radiusScaleOverride));
    gpuParams_.kernelSize = gpuParams_.radius * 2 + 1;
    gpuParams_.initCoverage = params_.value("initCoverage", 0.4f);
    gpuParams_.initScale = params_.value("initScale", 24.0f);
    gpuParams_.width = simWidth_;
    gpuParams_.height = simHeight_;
    kernelShellMu_ = params_.value("kernelShellMu", 0.5f);
    kernelShellSigma_ = params_.value("kernelShellSigma", 0.15f);

    // Lenia looks best on dark→bright organic palettes; override per scene.
    colorMap_.params.dR = 0.60f;
    colorMap_.params.dG = 0.45f;
    colorMap_.params.dB = 0.30f;
    colorMap_.params.channel = 0;
    colorMap_.params.inputScale = 1.0f;
    colorMap_.configure(params_);

    buildKernelTexture(ctx);

    if (organismLoaded_) {
        orgWidth_ = organism.width;
        orgHeight_ = organism.height;
        TextureDesc gd;
        gd.width = orgWidth_;
        gd.height = orgHeight_;
        gd.format = PixelFormat::R32F;
        gd.storage = StorageMode::Shared; // CPU-uploaded, same pattern as kernel_ above
        gd.label = instanceName_ + ".organismCells";
        organismCells_ = ctx.resources->createTexture(gd);
        ctx.resources->uploadTexture(organismCells_, organism.cells.data(),
                                     size_t(orgWidth_) * sizeof(float));
    }

    if (useFFT_) {
        TextureDesc cd;
        cd.width = simWidth_;
        cd.height = simHeight_;
        cd.format = PixelFormat::RG32F;
        cd.storage = StorageMode::GPUPrivate;
        cd.label = instanceName_ + ".fftPingA";
        fftPingA_ = ctx.resources->createTexture(cd);
        cd.label = instanceName_ + ".fftPingB";
        fftPingB_ = ctx.resources->createTexture(cd);
        cd.label = instanceName_ + ".stateFFT";
        stateFFTStable_ = ctx.resources->createTexture(cd);
        for (uint32_t k = 0; k < 4; ++k) {
            cd.label = instanceName_ + ".kernelFFT" + std::to_string(k);
            kernelFFT_[k] = ctx.resources->createTexture(cd);
        }

        TextureDesc pd;
        pd.width = simWidth_;
        pd.height = simHeight_;
        pd.format = PixelFormat::R32F;
        pd.storage = StorageMode::GPUPrivate;
        for (uint32_t k = 0; k < 4; ++k) {
            pd.label = instanceName_ + ".potential" + std::to_string(k);
            potential_[k] = ctx.resources->createTexture(pd);
        }

        TextureDesc ud;
        ud.width = simWidth_;
        ud.height = simHeight_;
        ud.format = PixelFormat::R32F;
        ud.storage = StorageMode::Shared; // CPU-uploaded kernel images (§3 needsInit)
        ud.label = instanceName_ + ".kernelImageUpload";
        kernelImageUpload_ = ctx.resources->createTexture(ud);
    }
}

void LeniaModule::buildKernelTexture(SimulationContext& ctx) {
    // Radial gaussian shell K(r) = bell(r, mu, sigma), r normalized to [0,1],
    // normalized so the kernel sums to 1 (potential u stays in [0,1] for
    // state in [0,1]).
    const uint32_t R = gpuParams_.radius;
    const uint32_t size = gpuParams_.kernelSize;
    std::vector<float> weights(size_t(size) * size, 0.0f);

    double sum = 0.0;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = float(int(x) - int(R));
            float dy = float(int(y) - int(R));
            float r = std::sqrt(dx * dx + dy * dy) / float(R);
            float w = 0.0f;
            if (r <= 1.0f && r > 0.0f) {
                float t = (r - kernelShellMu_) / kernelShellSigma_;
                w = std::exp(-0.5f * t * t);
            }
            weights[size_t(y) * size + x] = w;
            sum += w;
        }
    }
    if (sum > 0.0) {
        float inv = float(1.0 / sum);
        for (auto& w : weights) w *= inv;
    }

    TextureDesc kd;
    kd.width = size;
    kd.height = size;
    kd.format = PixelFormat::R32F;
    kd.storage = StorageMode::Shared; // CPU-uploaded LUT
    kd.label = instanceName_ + ".kernel";
    kernel_ = ctx.resources->createTexture(kd);
    ctx.resources->uploadTexture(kernel_, weights.data(), size * sizeof(float));
}

void LeniaModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x4C454E31); // "LEN1"
    needsInit_ = true;
}

void LeniaModule::updateCPU(const AudioFeatureState& audio) { audio_ = audio; }

void LeniaModule::encode(SimulationContext& ctx) {
    gpuParams_.dt = param(ctx, "dt", fallbackDt_);
    gpuParams_.growthMu = param(ctx, "growthMu", fallbackGrowthMu_);
    gpuParams_.growthSigma = param(ctx, "growthSigma", fallbackGrowthSigma_);
    gpuParams_.noiseAmount = param(ctx, "noiseAmount", 0.0f);
    gpuParams_.muJitter = param(ctx, "muJitter", 0.0f);
    gpuParams_.injectAmount = param(ctx, "injectAmount", 0.0f);
    gpuParams_.injectCount = uint32_t(param(ctx, "injectCount", 3.0f) + 0.5f);
    gpuParams_.frameIndex = ctx.frameIndex;
    gpuParams_.growthMode = uint32_t(param(ctx, "growthMode", 1.0f) + 0.5f);

    AudioUniforms au = toAudioUniforms(audio_);

    if (needsInit_) {
        gpuParams_.seed = seed_;
        // phase11: stamp a known organism's cells instead of noise when
        // initMode="stamp" resolved to a successfully-loaded organism file
        // (setup()); leniaInit itself is untouched either way (backward-
        // compat contract, phase9 comment above).
        if (useStampInit_) {
            LeniaStampParams sp;
            sp.width = simWidth_;
            sp.height = simHeight_;
            sp.orgWidth = orgWidth_;
            sp.orgHeight = orgHeight_;
            sp.stampCount = stampCount_;
            sp.stampRotate = stampRotate_ ? 1u : 0u;
            sp.seed = seed_;
            ctx.graph->pass(instanceName_ + ".stampInit")
                .pipeline("leniaStampInit")
                .write(0, field_.read())
                .read(1, organismCells_)
                .uniforms(0, sp)
                .dispatch2D(simWidth_, simHeight_);
        } else {
            ctx.graph->pass(instanceName_ + ".init")
                .pipeline("leniaInit")
                .write(0, field_.read())
                .uniforms(0, gpuParams_)
                .dispatch2D(simWidth_, simHeight_);
        }

        if (useFFT_) {
            // §3: precompute kernelFFT[k] once (GPU, needsInit only). No
            // "kernels" param -> one implicit kernel from the module's own
            // radius/growthMu/growthSigma (betas=[1], radiusScale=1) so
            // convMode=fft alone (no "kernels") still behaves like a single
            // radial-kernel Lenia, just convolved via FFT instead of direct.
            std::vector<KernelDef> effective = kernels_;
            if (effective.empty()) {
                KernelDef kd;
                kd.radiusScale = 1.0f;
                kd.mu = gpuParams_.growthMu;
                kd.sigma = gpuParams_.growthSigma;
                kd.weight = 1.0f;
                kd.betas = {1.0f};
                effective.push_back(kd);
            }
            uint32_t numK = std::min<uint32_t>(4, uint32_t(effective.size()));
            multiParams_.numKernels = numK;

            std::vector<float> img;
            FFTParams fb = fftBounds(simWidth_, simHeight_);
            for (uint32_t k = 0; k < numK; ++k) {
                float Rk = float(gpuParams_.radius) * effective[k].radiusScale;
                buildRingKernelImage(simWidth_, simHeight_, Rk, kernelShellSigma_,
                                     effective[k].betas, img);
                ctx.resources->uploadTexture(kernelImageUpload_, img.data(),
                                             size_t(simWidth_) * sizeof(float));

                ctx.graph->pass(instanceName_ + ".kimg" + std::to_string(k))
                    .pipeline("realToComplex")
                    .read(0, kernelImageUpload_)
                    .write(1, fftPingA_)
                    .uniforms(0, fb)
                    .dispatch2D(simWidth_, simHeight_);

                TextureHandle kfwd =
                    encodeFFT2D(*ctx.graph, instanceName_ + ".kfwd" + std::to_string(k),
                               fftPingA_, fftPingB_, simWidth_, simHeight_, +1);
                ctx.graph->copyTexture(kfwd, kernelFFT_[k]);

                multiParams_.kMu[k] = effective[k].mu;
                multiParams_.kSigma[k] = effective[k].sigma;
                multiParams_.kWeight[k] = effective[k].weight;
            }
            for (uint32_t k = numK; k < 4; ++k) {
                multiParams_.kMu[k] = 0.15f;
                multiParams_.kSigma[k] = 0.017f;
                multiParams_.kWeight[k] = 0.0f;
            }

            // Unused potential slots (k >= numK) must never contain
            // uninitialized GPU memory (kWeight=0 only zeroes their
            // contribution if the bound value is finite) — clear all 4 once;
            // slots < numK get fully overwritten every substep below anyway.
            for (uint32_t k = 0; k < 4; ++k) {
                ctx.graph->pass(instanceName_ + ".pclear" + std::to_string(k))
                    .pipeline("clearR32F")
                    .write(0, potential_[k])
                    .uniforms(0, fb)
                    .dispatch2D(simWidth_, simHeight_);
            }
        }
        needsInit_ = false;
    }

    uint32_t steps = std::max(1u, ctx.substeps);
    for (uint32_t s = 0; s < steps; ++s) {
        gpuParams_.seed = pcgHash(seed_ ^ (ctx.frameIndex * 197u + s));

        if (!useFFT_) {
            ctx.graph->pass(instanceName_ + ".step")
                .pipeline("leniaStep")
                .read(0, field_.read())
                .write(1, field_.write())
                .read(2, kernel_)
                .uniforms(0, gpuParams_)
                .uniforms(1, au)
                .dispatch2D(simWidth_, simHeight_);
            field_.swap();
        } else {
            // §3 per-frame FFT convolution: realToComplex -> FFT2D forward
            // (state) -> per kernel: complexMulScale -> FFT2D inverse ->
            // complexToReal -> potential_k -> leniaGrowthMulti.
            FFTParams fb = fftBounds(simWidth_, simHeight_);

            ctx.graph->pass(instanceName_ + ".r2c" + std::to_string(s))
                .pipeline("realToComplex")
                .read(0, field_.read())
                .write(1, fftPingA_)
                .uniforms(0, fb)
                .dispatch2D(simWidth_, simHeight_);

            TextureHandle sfwd =
                encodeFFT2D(*ctx.graph, instanceName_ + ".sfwd" + std::to_string(s), fftPingA_,
                           fftPingB_, simWidth_, simHeight_, +1);
            // stateFFTStable_ must survive the per-kernel loop below, which
            // reuses fftPingA_/fftPingB_ as scratch — copy it out first.
            ctx.graph->copyTexture(sfwd, stateFFTStable_);

            for (uint32_t k = 0; k < multiParams_.numKernels; ++k) {
                std::string kk = std::to_string(s) + "_" + std::to_string(k);
                ctx.graph->pass(instanceName_ + ".cmul" + kk)
                    .pipeline("complexMulScale")
                    .read(0, stateFFTStable_)
                    .read(1, kernelFFT_[k])
                    .write(2, fftPingA_)
                    // normalize=0: the inverse FFT2D below applies 1/(W*H)
                    // via its own per-stage normalize flag (§2 comment).
                    .uniforms(0, fb)
                    .dispatch2D(simWidth_, simHeight_);

                TextureHandle inv = encodeFFT2D(*ctx.graph, instanceName_ + ".inv" + kk,
                                               fftPingA_, fftPingB_, simWidth_, simHeight_, -1);

                ctx.graph->pass(instanceName_ + ".c2r" + kk)
                    .pipeline("complexToReal")
                    .read(0, inv)
                    .write(1, potential_[k])
                    .uniforms(0, fb)
                    .dispatch2D(simWidth_, simHeight_);
            }

            multiParams_.dt = gpuParams_.dt;
            multiParams_.growthMode = gpuParams_.growthMode;
            multiParams_.noiseAmount = gpuParams_.noiseAmount;
            multiParams_.muJitter = gpuParams_.muJitter;
            multiParams_.injectAmount = gpuParams_.injectAmount;
            multiParams_.injectCount = gpuParams_.injectCount;
            multiParams_.radius = gpuParams_.radius;
            multiParams_.seed = gpuParams_.seed;
            multiParams_.width = simWidth_;
            multiParams_.height = simHeight_;
            multiParams_.frameIndex = gpuParams_.frameIndex;

            ctx.graph->pass(instanceName_ + ".growth" + std::to_string(s))
                .pipeline("leniaGrowthMulti")
                .read(0, field_.read())
                .write(1, field_.write())
                .read(2, potential_[0])
                .read(3, potential_[1])
                .read(4, potential_[2])
                .read(5, potential_[3])
                .uniforms(0, multiParams_)
                .uniforms(1, au)
                .dispatch2D(simWidth_, simHeight_);
            field_.swap();
        }
    }

    if (simWidth_ == width_ && simHeight_ == height_) {
        colorMap_.encode(*ctx.graph, instanceName_ + ".colorMap", field_.read(), output_,
                         width_, height_);
    } else {
        colorMap_.encodeScaled(*ctx.graph, instanceName_ + ".colorMap", field_.read(), output_,
                               simWidth_, simHeight_, width_, height_);
    }
}

} // namespace life
