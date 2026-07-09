// LifeCore/Output/SyphonSink.mm
// Metal/Syphon live here, per the phase 6 spec's carve-out: Output-layer .mm
// files may use Metal directly (MetalInternal.h).
#include "LifeCore/Output/SyphonSink.h"
#include "LifeCore/Metal/MetalInternal.h"

#import <Syphon/SyphonMetalServer.h>
#import <IOSurface/IOSurface.h>
#import <CoreFoundation/CoreFoundation.h>

#include <cstdio>

namespace life {

struct SyphonSink::Impl {
    SyphonMetalServer* server = nil;
    uint32_t width = 0;
    uint32_t height = 0;
    bool stopped = false;
};

SyphonSink::SyphonSink(std::string serverName)
    : impl_(std::make_unique<Impl>()), serverName_(std::move(serverName)) {}

SyphonSink::~SyphonSink() { stop(); }

bool SyphonSink::start(MetalContext& metal, ResourcePool& /*pool*/, uint32_t width,
                       uint32_t height, std::string& outError) {
    @autoreleasepool {
        NSString* name = [NSString stringWithUTF8String:serverName_.c_str()];
        impl_->server = [[SyphonMetalServer alloc] initWithName:name
                                                          device:metal.impl().device
                                                         options:nil];
        if (!impl_->server) {
            outError =
                "SyphonMetalServer initWithName:device:options: returned nil (server name '" +
                serverName_ + "')";
            return false;
        }
        impl_->width = width;
        impl_->height = height;
    }
    return true;
}

void SyphonSink::publish(CommandGraph& graph, TextureHandle frame) {
    @autoreleasepool {
        if (!impl_->server) return;

        id<MTLTexture> src = graph.resources().impl().texture(frame);
        if (!src) return;

        id<MTLCommandBuffer> cb = graph.impl().commandBuffer;
        if (!cb) return;

        // Publish the RGBA16F render target directly (design doc phase 6 §5,
        // primary path). SyphonMetalServer copies/blits into its own
        // BGRA8Unorm-backed IOSurface texture internally.
        NSRect region = NSMakeRect(0, 0, src.width, src.height);
        [impl_->server publishFrameTexture:src onCommandBuffer:cb imageRegion:region flipped:NO];
    }
}

void SyphonSink::pump(bool& /*shouldQuit*/) {
    // SyphonServerBase discovers/announces itself over
    // NSDistributedNotificationCenter, which only delivers to an observer
    // while that observer's thread services its run loop. LifeRealtime's
    // main loop otherwise never spins one (no NSApplication run, no
    // CFRunLoopRun), so without this a server started here would never
    // answer a late client's discovery request (SyphonServerAnnounceRequest)
    // — confirmed necessary empirically via SyphonCheck end-to-end. Drain
    // whatever is already pending; never block if there is nothing to do.
    while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource) {
    }
}

void SyphonSink::stop() {
    if (impl_->stopped) return;
    impl_->stopped = true;
    @autoreleasepool {
        if (impl_->server) {
            [impl_->server stop];
            impl_->server = nil;
        }
    }
}

} // namespace life
