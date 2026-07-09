#pragma once
// LifeCore/Output/OutputSink.h
// Pure C++ output abstraction (design doc §3.8: "出力抽象化は用意する").
// Concrete sinks (window preview, Syphon, ...) live in .mm files and are
// free to use Metal/AppKit there; this header must stay Metal-free so it is
// includable from any .cpp (SceneRunner.h, the app main()s, ...).

#include "LifeCore/Metal/Handles.h"

#include <cstdint>
#include <string>

namespace life {

class MetalContext;
class ResourcePool;
class CommandGraph;

class OutputSink {
public:
    virtual ~OutputSink() = default;

    // Called once, synchronously, from SceneRunner::addOutputSink (create
    // after / run before). Return false and fill outError to abort
    // registration; the caller logs a warning and discards the sink without
    // otherwise disturbing the app.
    virtual bool start(MetalContext& metal, ResourcePool& pool, uint32_t width,
                       uint32_t height, std::string& outError) = 0;

    // Called while the frame's command buffer is open (after beginFrame,
    // before endFrame) — specifically right after composite encode and
    // before any readback encode. Implementations encode blit/compute/
    // present work against `frame` (the RGBA16F final render target) onto
    // `graph`'s open command buffer. Must not commit or wait on the buffer.
    virtual void publish(CommandGraph& graph, TextureHandle frame) = 0;

    // Called once per app loop iteration, outside the frame (event pumps,
    // window-close detection, run loop service, ...). Default: no-op.
    virtual void pump(bool& shouldQuit) { (void)shouldQuit; }

    // Release any resources the sink holds. Safe to call more than once and
    // safe to skip if start() never succeeded.
    virtual void stop() {}

    virtual const char* name() const = 0;
};

} // namespace life
