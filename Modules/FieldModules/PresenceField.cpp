// Modules/FieldModules/PresenceField.cpp
#include "Modules/FieldModules/PresenceField.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace life {

void PresenceFieldModule::setup(SimulationContext& ctx) {
    width_ = ctx.width;
    height_ = ctx.height;

    Field2DDesc fd;
    fd.width = width_;
    fd.height = height_;
    fd.format = PixelFormat::R16F;
    fd.pingPong = true;
    fd.boundary = BoundaryMode::Wrap; // x wraps (walk the 93m strip); y-clamp is handled in-kernel
    fd.label = instanceName_ + ".field";
    field_.create(*ctx.resources, fd);

    TextureDesc od;
    od.width = width_;
    od.height = height_;
    od.format = PixelFormat::RGBA16F;
    od.label = instanceName_ + ".output";
    output_ = ctx.resources->createTexture(od);

    gpuParams_.width = width_;
    gpuParams_.height = height_;

    colorMap_.params.channel = 0;
    colorMap_.params.inputScale = 1.0f;
    colorMap_.configure(params_);
}

void PresenceFieldModule::reset(uint32_t seed) {
    (void)seed; // no randomness in this module — presence is externally driven
    needsInit_ = true;
    watchdogTripped_ = false;
}

void PresenceFieldModule::updateCPU(const AudioFeatureState& audio) { presence_ = audio.presence; }

void PresenceFieldModule::encode(SimulationContext& ctx) {
    const float radiusM = param(ctx, "radius", 1.2f);
    const float pixelsPerMeter = param(ctx, "pixelsPerMeter", 176.1f); // 16380px / 93.0m
    const float holdSeconds = param(ctx, "holdSeconds", 0.5f);
    const float watchdogSeconds = param(ctx, "watchdogSeconds", 3.0f);
    const float strengthScale = param(ctx, "strengthScale", 1.0f);

    gpuParams_.radiusPx = radiusM * pixelsPerMeter;
    gpuParams_.attack = param(ctx, "attack", 6.0f);
    gpuParams_.release = param(ctx, "release", 1.5f);
    gpuParams_.dt = ctx.dt;
    gpuParams_.strengthScale = strengthScale;

    // Watchdog: no /presence or /presence/clear at all for watchdogSeconds
    // means the OSC link itself is suspect, independent of any individual
    // point's own hold timer (which, with default settings, would already
    // have dropped every point well before this trips — the watchdog is the
    // outer safety net for a scene tuned with a larger holdSeconds, or for
    // any bug that keeps refreshing one id while the rest of the feed is
    // dead). Tripping forces the target to zero this frame regardless of
    // any points that technically haven't aged past holdSeconds yet; the
    // module's own release-rate smoothing does the actual "fade smoothly".
    const bool watchdogOk = presence_.secondsSinceMessage <= watchdogSeconds;
    if (!watchdogOk && !watchdogTripped_) {
        fprintf(stderr,
                "[life] %s: presence watchdog tripped (no /presence for >%.1fs) — fading to zero\n",
                instanceName_.c_str(), watchdogSeconds);
        watchdogTripped_ = true;
    } else if (watchdogOk && watchdogTripped_) {
        fprintf(stderr, "[life] %s: presence resumed\n", instanceName_.c_str());
        watchdogTripped_ = false;
    }

    uint32_t kept = 0;
    if (watchdogOk) {
        for (uint32_t i = 0; i < presence_.count && kept < kMaxPresencePoints; ++i) {
            const PresencePoint& pt = presence_.points[i];
            if (pt.age > holdSeconds) continue; // sender stopped refreshing this id
            float xn = pt.x - std::floor(pt.x);             // wrap into [0,1) defensively
            float yn = std::clamp(pt.y, 0.0f, 1.0f);
            gpuPoints_.data[kept * 4 + 0] = xn * float(width_);
            gpuPoints_.data[kept * 4 + 1] = yn * float(height_);
            gpuPoints_.data[kept * 4 + 2] = pt.strength * strengthScale;
            gpuPoints_.data[kept * 4 + 3] = 0.0f;
            kept++;
        }
    }
    gpuParams_.pointCount = kept;

    if (needsInit_) {
        ctx.graph->pass(instanceName_ + ".clear")
            .pipeline("presenceFieldClear")
            .write(0, field_.read())
            .uniforms(0, gpuParams_)
            .dispatch2D(width_, height_);
        needsInit_ = false;
    }

    ctx.graph->pass(instanceName_ + ".update")
        .pipeline("presenceFieldUpdate")
        .read(0, field_.read())
        .write(1, field_.write())
        .uniforms(0, gpuParams_)
        .uniforms(1, gpuPoints_)
        .dispatch2D(width_, height_);
    field_.swap();

    colorMap_.encode(*ctx.graph, instanceName_ + ".colorMap", field_.read(), output_, width_,
                     height_, ctx.dt);
}

} // namespace life
