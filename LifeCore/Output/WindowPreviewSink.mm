// LifeCore/Output/WindowPreviewSink.mm
// Metal/AppKit live here, per the phase 6 spec's carve-out: Output-layer .mm
// files may use Metal directly (MetalInternal.h) — that is the intended way
// to keep Metal out of .h files while still touching CommandGraph::Impl and
// ResourcePool::Impl.
#include "LifeCore/Output/WindowPreviewSink.h"
#include "LifeCore/Metal/MetalInternal.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cstdio>

// Mirrors Shaders/Render/Present.metal's PresentParams (uniform structs are
// hand-mirrored between C++/ObjC++ and MSL throughout this codebase).
namespace {
struct PresentParams {
    uint32_t width;
    uint32_t height;
    float exposure;
};
} // namespace

// Detects the user closing the preview window so the app can shut down
// gracefully instead of rendering into a dead layer.
@interface LifeWindowCloseWatcher : NSObject <NSWindowDelegate>
@property(atomic, assign) BOOL closed;
@end

@implementation LifeWindowCloseWatcher
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    self.closed = YES;
}
@end

namespace life {

struct WindowPreviewSink::Impl {
    id<MTLDevice> device = nil;
    std::unique_ptr<PipelineCache> pipelines; // presentToBGRA cache, owned by this sink
    NSWindow* window = nil;
    CAMetalLayer* layer = nil;
    LifeWindowCloseWatcher* closeWatcher = nil;
    uint32_t width = 0;
    uint32_t height = 0;
    bool stopped = false;
};

WindowPreviewSink::WindowPreviewSink(std::string sceneName, float previewScale)
    : impl_(std::make_unique<Impl>()),
      sceneName_(std::move(sceneName)),
      previewScale_(previewScale > 0.0f ? previewScale : 0.5f) {}

WindowPreviewSink::~WindowPreviewSink() { stop(); }

bool WindowPreviewSink::start(MetalContext& metal, ResourcePool& /*pool*/, uint32_t width,
                              uint32_t height, std::string& outError) {
    @autoreleasepool {
        impl_->device = metal.impl().device;
        impl_->width = width;
        impl_->height = height;

        // Own a private PipelineCache purely to resolve/cache "presentToBGRA"
        // — it shares MetalContext's single compiled library (Present.metal
        // is concatenated into it like every other Shaders/ source), so this
        // finds the same pipeline SyphonSink would build independently.
        impl_->pipelines = std::make_unique<PipelineCache>(metal);
        std::string pipelineErr;
        if (!impl_->pipelines->impl().pipeline("presentToBGRA", pipelineErr)) {
            outError = "WindowPreviewSink: " + pipelineErr;
            impl_->pipelines.reset();
            return false;
        }

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect contentRect =
            NSMakeRect(0, 0, std::max(1.0f, width * previewScale_),
                      std::max(1.0f, height * previewScale_));
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        impl_->window = [[NSWindow alloc] initWithContentRect:contentRect
                                                     styleMask:style
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
        if (!impl_->window) {
            outError = "WindowPreviewSink: NSWindow allocation failed";
            return false;
        }
        [impl_->window
            setTitle:[NSString stringWithFormat:@"LifeRealtime — %s", sceneName_.c_str()]];
        impl_->closeWatcher = [LifeWindowCloseWatcher new];
        [impl_->window setDelegate:impl_->closeWatcher];
        [impl_->window setReleasedWhenClosed:NO];

        NSView* contentView = [impl_->window contentView];
        [contentView setWantsLayer:YES];

        impl_->layer = [CAMetalLayer layer];
        impl_->layer.device = impl_->device;
        impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        impl_->layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        impl_->layer.drawableSize = CGSizeMake(width, height);
        // Required: compute-kernel texture writes need this, unlike the
        // default (framebufferOnly=YES) render-only configuration.
        impl_->layer.framebufferOnly = NO;
        [contentView setLayer:impl_->layer];

        [impl_->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    return true;
}

void WindowPreviewSink::publish(CommandGraph& graph, TextureHandle frame) {
    @autoreleasepool {
        if (!impl_->layer) return;

        id<CAMetalDrawable> drawable = [impl_->layer nextDrawable];
        if (!drawable) return; // busy — skip this frame rather than stall

        id<MTLTexture> src = graph.resources().impl().texture(frame);
        if (!src) return;

        std::string err;
        id<MTLComputePipelineState> pso = impl_->pipelines->impl().pipeline("presentToBGRA", err);
        if (!pso) {
            fprintf(stderr, "[life] WindowPreviewSink: %s\n", err.c_str());
            return;
        }

        id<MTLCommandBuffer> cb = graph.impl().commandBuffer;
        if (!cb) return;

        // Raw encoder — deliberately bypasses the ComputePass describe/
        // dispatch helper so we can bind the drawable's texture directly.
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        enc.label = @"present.window";
        [enc setComputePipelineState:pso];
        [enc setTexture:src atIndex:0];
        [enc setTexture:drawable.texture atIndex:1];
        PresentParams params{impl_->width, impl_->height, 1.0f};
        [enc setBytes:&params length:sizeof(params) atIndex:0];

        NSUInteger tw = pso.threadExecutionWidth;
        NSUInteger th = std::max<NSUInteger>(1, pso.maxTotalThreadsPerThreadgroup / tw);
        [enc dispatchThreads:MTLSizeMake(impl_->width, impl_->height, 1)
            threadsPerThreadgroup:MTLSizeMake(tw, std::min<NSUInteger>(th, 16), 1)];
        [enc endEncoding];

        [cb presentDrawable:drawable];
    }
}

void WindowPreviewSink::pump(bool& shouldQuit) {
    @autoreleasepool {
        NSEvent* event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES]) != nil) {
            [NSApp sendEvent:event];
        }
        if (impl_->closeWatcher.closed) shouldQuit = true;
    }
}

void WindowPreviewSink::stop() {
    if (impl_->stopped) return;
    impl_->stopped = true;
    @autoreleasepool {
        if (impl_->window) {
            [impl_->window setDelegate:nil];
            [impl_->window close];
            impl_->window = nil;
        }
        impl_->layer = nil;
        impl_->closeWatcher = nil;
        impl_->pipelines.reset();
    }
}

} // namespace life
