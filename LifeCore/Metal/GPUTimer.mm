// LifeCore/Metal/GPUTimer.mm
// Stage-boundary counter sampling. On Apple Silicon the timestamp counter set
// supports sampling at compute encoder boundaries; timestamps are calibrated
// to nanoseconds via sampleTimestamps pairs.
#include "LifeCore/Metal/MetalInternal.h"

namespace life {

void GPUTimer::Impl::calibrate() {
    // Two correlated CPU/GPU samples give us ns-per-tick. On current Apple
    // GPUs timestamps are already normalized to nanoseconds (scale ~1.0), but
    // calibrating keeps us correct across drivers.
    MTLTimestamp cpu0 = 0, gpu0 = 0, cpu1 = 0, gpu1 = 0;
    [ctx->impl().device sampleTimestamps:&cpu0 gpuTimestamp:&gpu0];
    // Busy-wait a tiny bit so the deltas are nonzero.
    for (volatile int i = 0; i < 100000; ++i) {}
    [ctx->impl().device sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
    if (gpu1 > gpu0 && cpu1 > cpu0) {
        // CPU timestamps are in nanoseconds (mach_absolute_time normalized).
        gpuTimebaseScale = double(cpu1 - cpu0) / double(gpu1 - gpu0);
    } else {
        gpuTimebaseScale = 1.0;
    }
}

void GPUTimer::Impl::beginFrame() { pendingLabels.clear(); }

uint32_t GPUTimer::Impl::registerPass(const std::string& label) {
    if (!supported) return UINT32_MAX;
    if (pendingLabels.size() >= kMaxPasses) return UINT32_MAX;
    pendingLabels.push_back(label);
    return static_cast<uint32_t>((pendingLabels.size() - 1) * 2);
}

void GPUTimer::Impl::resolve(double wholeBufferMs) {
    lastTimings.clear();
    lastTotalMs = wholeBufferMs;
    if (!supported || pendingLabels.empty()) return;

    @autoreleasepool {
        NSRange range = NSMakeRange(0, pendingLabels.size() * 2);
        NSData* data = [sampleBuffer resolveCounterRange:range];
        if (!data) return;
        const auto* stamps =
            reinterpret_cast<const MTLCounterResultTimestamp*>(data.bytes);
        size_t count = data.length / sizeof(MTLCounterResultTimestamp);
        for (size_t i = 0; i < pendingLabels.size(); ++i) {
            if (i * 2 + 1 >= count) break;
            uint64_t t0 = stamps[i * 2].timestamp;
            uint64_t t1 = stamps[i * 2 + 1].timestamp;
            PassTiming t;
            t.label = pendingLabels[i];
            if (t1 > t0 && t0 != MTLCounterErrorValue && t1 != MTLCounterErrorValue) {
                t.gpuMilliseconds = double(t1 - t0) * gpuTimebaseScale * 1e-6;
                t.exact = true;
            } else {
                t.gpuMilliseconds = 0.0;
                t.exact = false;
            }
            lastTimings.push_back(std::move(t));
        }
    }
}

GPUTimer::GPUTimer(MetalContext& ctx) : impl_(std::make_unique<Impl>()) {
    impl_->ctx = &ctx;
    @autoreleasepool {
        id<MTLDevice> device = ctx.impl().device;
        if (!ctx.impl().enableGPUTiming) return;
        if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
            return;
        for (id<MTLCounterSet> set in device.counterSets) {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
                impl_->timestampCounterSet = set;
                break;
            }
        }
        if (!impl_->timestampCounterSet) return;

        MTLCounterSampleBufferDescriptor* desc = [MTLCounterSampleBufferDescriptor new];
        desc.counterSet = impl_->timestampCounterSet;
        desc.storageMode = MTLStorageModeShared;
        desc.sampleCount = Impl::kMaxPasses * 2;
        desc.label = @"LifeCore.GPUTimer";
        NSError* error = nil;
        impl_->sampleBuffer = [device newCounterSampleBufferWithDescriptor:desc
                                                                     error:&error];
        if (!impl_->sampleBuffer) return;

        impl_->supported = true;
        ctx.impl().timingSupported = true;
        impl_->calibrate();
    }
}

GPUTimer::~GPUTimer() = default;

bool GPUTimer::counterSamplingSupported() const { return impl_->supported; }

const std::vector<PassTiming>& GPUTimer::lastFrameTimings() const {
    return impl_->lastTimings;
}

double GPUTimer::lastFrameTotalMs() const { return impl_->lastTotalMs; }

} // namespace life
