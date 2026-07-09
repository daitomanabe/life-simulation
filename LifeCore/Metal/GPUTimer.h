#pragma once
// LifeCore/Metal/GPUTimer.h
// Per-pass GPU timing via MTLCounterSampleBuffer (stage-boundary sampling on
// Apple GPUs), with graceful fallback to whole-command-buffer timing when
// counter sampling is unavailable. Every pass gets a timing entry (design doc
// §22-16).

#include <memory>
#include <string>
#include <vector>

namespace life {

class MetalContext;

struct PassTiming {
    std::string label;
    double gpuMilliseconds = 0.0;
    bool exact = true; // false when derived from fallback whole-buffer timing
};

class GPUTimer {
public:
    explicit GPUTimer(MetalContext& ctx);
    ~GPUTimer();

    GPUTimer(const GPUTimer&) = delete;
    GPUTimer& operator=(const GPUTimer&) = delete;

    bool counterSamplingSupported() const;

    // Results for the most recently completed frame.
    const std::vector<PassTiming>& lastFrameTimings() const;
    double lastFrameTotalMs() const;

    struct Impl;
    Impl& impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace life
