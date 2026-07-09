// Modules/FieldModules/Fluid.cpp
#include "Modules/FieldModules/Fluid.h"

#include "LifeCore/Math/Random.h"

#include <algorithm>

namespace life {

namespace {
// Mirrors FluidPresentParams in Shaders/Field/Fluid.metal — the dye->output
// exposure copy is its own tiny uniform, not part of FluidParams (same
// pattern as Boids.cpp's local SplatResolveRGBParams/ClearParams: a small
// param block for one specific pass, defined right where it's used).
struct FluidPresentParams {
    uint32_t width;
    uint32_t height;
    float exposure;
};
} // namespace

void FluidModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    Field2DDesc vd;
    vd.width = width_;
    vd.height = height_;
    vd.format = PixelFormat::RG32F;
    vd.pingPong = true;
    vd.boundary = BoundaryMode::Wrap;
    vd.label = instanceName_ + ".velocity";
    velocity_.create(*ctx.resources, vd);

    Field2DDesc dd;
    dd.width = width_;
    dd.height = height_;
    dd.format = PixelFormat::RGBA16F;
    dd.pingPong = true;
    dd.boundary = BoundaryMode::Wrap;
    dd.label = instanceName_ + ".dye";
    dye_.create(*ctx.resources, dd);

    Field2DDesc pd;
    pd.width = width_;
    pd.height = height_;
    pd.format = PixelFormat::R32F;
    pd.pingPong = true;
    pd.boundary = BoundaryMode::Wrap;
    pd.label = instanceName_ + ".pressure";
    pressure_.create(*ctx.resources, pd);

    // divergence is single-buffered (no ping-pong): fluidDivergence fully
    // overwrites it every frame before Jacobi ever reads it.
    Field2DDesc gd;
    gd.width = width_;
    gd.height = height_;
    gd.format = PixelFormat::R32F;
    gd.pingPong = false;
    gd.boundary = BoundaryMode::Wrap;
    gd.label = instanceName_ + ".divergence";
    divergence_.create(*ctx.resources, gd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    gpuParams_.width = width_;
    gpuParams_.height = height_;

    // 4x4 black fallback for the forceField input port (Phase 12) — same
    // rationale as the other modules' coupling fallbacks.
    TextureDesc fbd;
    fbd.width = 4;
    fbd.height = 4;
    fbd.format = PixelFormat::R16F;
    fbd.storage = StorageMode::Shared;
    fbd.label = instanceName_ + ".forceFieldFallback";
    forceFieldFallback_ = ctx.resources->createTexture(fbd);
    std::vector<uint8_t> zeros(size_t(fbd.width) * fbd.height * bytesPerPixel(fbd.format), 0);
    ctx.resources->uploadTexture(forceFieldFallback_, zeros.data(),
                                 size_t(fbd.width) * bytesPerPixel(fbd.format));
    forceFieldInput_ = forceFieldFallback_;
}

void FluidModule::reset(uint32_t seed) {
    seed_ = deriveSeed(seed, 0x464C5531); // "FLU1"
    needsInit_ = true;
}

void FluidModule::updateCPU(const AudioFeatureState& audio) { audio_ = audio; }

void FluidModule::encode(SimulationContext& ctx) {
    gpuParams_.dt = ctx.dt;
    gpuParams_.velDissipation = param(ctx, "velDissipation", 0.05f);
    gpuParams_.dyeDissipation = param(ctx, "dyeDissipation", 0.12f);
    gpuParams_.vorticity = param(ctx, "vorticity", 4.0f);
    gpuParams_.impulse = param(ctx, "impulse", 0.4f);
    gpuParams_.impulseRadius = param(ctx, "impulseRadius", 60.0f);
    gpuParams_.turbulence = param(ctx, "turbulence", 0.0f);
    gpuParams_.injectHue = param(ctx, "injectHue", 0.0f);
    gpuParams_.dyeInject = param(ctx, "dyeInject", 0.02f);
    gpuParams_.forceFieldGain = param(ctx, "forceFieldGain", 0.0f);
    // low/mid/high are transcribed straight from AudioFeatureState (design
    // point: not scene-base-value params, so no ParameterBus indirection).
    gpuParams_.low = audio_.low;
    gpuParams_.mid = audio_.mid;
    gpuParams_.high = audio_.high;
    gpuParams_.frameIndex = ctx.frameIndex;

    if (needsInit_) {
        gpuParams_.seed = seed_;
        // Writes the current read side of all three ping-pong fields so
        // this frame's substep loop sees an all-zero start (warm start for
        // every later frame comes from whatever the loop leaves behind).
        ctx.graph->pass(instanceName_ + ".clear")
            .pipeline("fluidClear")
            .write(0, velocity_.read())
            .write(1, dye_.read())
            .write(2, pressure_.read())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        needsInit_ = false;
    }

    uint32_t jacobiIterations =
        uint32_t(std::max(1.0f, param(ctx, "jacobiIterations", 28.0f) + 0.5f));

    // substeps (offline quality knob, §17) wraps the whole physics loop;
    // Present runs once per visual frame, after the loop (house convention).
    uint32_t steps = std::max(1u, ctx.substeps);
    for (uint32_t s = 0; s < steps; ++s) {
        // Deliberately NOT frameIndex-mixed (unlike Lenia/RD/Boids' per-frame
        // reseed): fluidForces/fluidAdvectDye hash the kick impulse position
        // from (p.seed, frameIndex/20) so it stays put for a whole cycle
        // window (§2 kernel 3 "12 フレーム毎に移動" — the point must be
        // stable across those frames, not re-randomized every frame). Only
        // the substep index decorrelates repeated substeps within one frame;
        // per-frame variety for turbulence/stirring comes from folding
        // p.frameIndex into THEIR rand01 tag instead (see Fluid.metal).
        gpuParams_.seed = pcgHash(seed_ ^ s);

        ctx.graph->pass(instanceName_ + ".advectVel")
            .pipeline("fluidAdvectVel")
            .read(0, velocity_.read())
            .write(1, velocity_.write())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        velocity_.swap();

        ctx.graph->pass(instanceName_ + ".forces")
            .pipeline("fluidForces")
            .read(0, velocity_.read())
            .write(1, velocity_.write())
            .read(2, forceFieldInput_)
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        velocity_.swap();

        ctx.graph->pass(instanceName_ + ".vorticity")
            .pipeline("fluidVorticity")
            .read(0, velocity_.read())
            .write(1, velocity_.write())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        velocity_.swap();

        ctx.graph->pass(instanceName_ + ".divergence")
            .pipeline("fluidDivergence")
            .read(0, velocity_.read())
            .write(1, divergence_.write())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);

        // Jacobi relaxation: pressure_ ping-pongs jacobiIterations times,
        // warm-started from last frame's converged pressure (no pre-clear).
        for (uint32_t j = 0; j < jacobiIterations; ++j) {
            ctx.graph->pass(instanceName_ + ".jacobi")
                .pipeline("fluidJacobi")
                .read(0, pressure_.read())
                .read(1, divergence_.read())
                .write(2, pressure_.write())
                .uniforms(0, gpuParams_)
                .dispatch2D(width_, height_);
            pressure_.swap();
        }

        ctx.graph->pass(instanceName_ + ".project")
            .pipeline("fluidProject")
            .read(0, velocity_.read())
            .read(1, pressure_.read())
            .write(2, velocity_.write())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        velocity_.swap();

        ctx.graph->pass(instanceName_ + ".advectDye")
            .pipeline("fluidAdvectDye")
            .read(0, dye_.read())
            .read(1, velocity_.read())
            .write(2, dye_.write())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        dye_.swap();
    }

    // dye -> output_: exposure-only copy (no ColorMapPass — dye is already
    // color). Runs once per visual frame, after the substep loop.
    FluidPresentParams pp{width_, height_, param(ctx, "exposure", 1.0f)};
    ctx.graph->pass(instanceName_ + ".present")
        .pipeline("fluidPresent")
        .read(0, dye_.read())
        .write(1, output_)
        .uniforms(0, pp)
        .dispatch2D(width_, height_);
}

} // namespace life
