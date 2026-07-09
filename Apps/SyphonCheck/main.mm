// Apps/SyphonCheck/main.mm
// Verification-only Syphon (Metal) client (phase 6 spec §6). Looks up a
// named server via the vendored SyphonServerDirectory, waits up to 5s for a
// frame, and reports success/failure on stdout with a matching process exit
// code. Deliberately a separate executable from LifeRealtime/SyphonSink —
// run in its own process so this is a real cross-process Syphon/IOSurface
// round trip, not an in-process sanity check.
//
// Usage: SyphonCheck <ServerName>
// Success: prints "syphon_check: got WxH frame" to stdout, exit 0.
// Failure: prints a reason to stderr, exit 1.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/SyphonMetalClient.h>
#import <Syphon/SyphonServerDirectory.h>

#include <cstdio>
#include <vector>

static const NSTimeInterval kOverallTimeoutSeconds = 5.0;
static const NSTimeInterval kPollIntervalSeconds = 0.05;

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: %s <ServerName>\n", argv[0]);
            return 1;
        }
        NSString* wantedName = [NSString stringWithUTF8String:argv[1]];
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:kOverallTimeoutSeconds];

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            fprintf(stderr, "syphon_check: MTLCreateSystemDefaultDevice() returned nil\n");
            return 1;
        }

        // --- 1. Discover the server. SyphonServerDirectory posts a
        // discovery request on first access and populates itself
        // asynchronously via NSDistributedNotificationCenter, so this must
        // spin the run loop while it waits (see SyphonSink::pump for the
        // matching requirement on the server side). ---
        NSDictionary<NSString*, id>* description = nil;
        while ([deadline timeIntervalSinceNow] > 0) {
            NSArray<NSDictionary<NSString*, id>*>* matches =
                [[SyphonServerDirectory sharedDirectory] serversMatchingName:wantedName
                                                                       appName:nil];
            if (matches.count > 0) {
                description = matches.firstObject;
                break;
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, kPollIntervalSeconds, false);
        }

        if (!description) {
            fprintf(stderr, "syphon_check: no server named '%s' found within %.0fs\n", argv[1],
                    kOverallTimeoutSeconds);
            return 1;
        }
        fprintf(stderr, "syphon_check: found server '%s' (uuid %s)\n", argv[1],
                [[description objectForKey:SyphonServerDescriptionUUIDKey] UTF8String]);

        // --- 2. Connect and wait for a frame. ---
        SyphonMetalClient* client =
            [[SyphonMetalClient alloc] initWithServerDescription:description
                                                            device:device
                                                           options:nil
                                                   newFrameHandler:nil];
        if (!client) {
            fprintf(stderr, "syphon_check: SyphonMetalClient init returned nil\n");
            return 1;
        }

        id<MTLTexture> frame = nil;
        while ([deadline timeIntervalSinceNow] > 0) {
            if (!client.isValid) {
                fprintf(stderr, "syphon_check: client became invalid while waiting\n");
                break;
            }
            if (client.hasNewFrame) {
                frame = [client newFrameImage];
                if (frame) break;
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, kPollIntervalSeconds, false);
        }

        if (!frame) {
            fprintf(stderr, "syphon_check: no frame received within %.0fs\n",
                    kOverallTimeoutSeconds);
            [client stop];
            return 1;
        }

        // Diagnostic only (not part of the pass/fail contract): sample a
        // mid-frame row so a silently-black publish path is easy to spot
        // from stderr instead of only inferring it from a "success" exit.
        {
            NSUInteger w = frame.width, h = frame.height;
            std::vector<uint8_t> row(w * 4);
            [frame getBytes:row.data()
                bytesPerRow:w * 4
                 fromRegion:MTLRegionMake2D(0, h / 2, w, 1)
                mipmapLevel:0];
            uint64_t sum = 0;
            for (uint8_t v : row) sum += v;
            fprintf(stderr,
                    "syphon_check: mid-row average byte value = %.2f (0 = black, BGRA8)\n",
                    row.empty() ? 0.0 : double(sum) / double(row.size()));
        }

        printf("syphon_check: got %lux%lu frame\n", (unsigned long)frame.width,
               (unsigned long)frame.height);
        [client stop];
        return 0;
    }
}
