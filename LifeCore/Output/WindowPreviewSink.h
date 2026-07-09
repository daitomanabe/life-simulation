#pragma once
// LifeCore/Output/WindowPreviewSink.h
// Pure C++ facade — Metal/AppKit types stay inside WindowPreviewSink.mm
// (design doc §3.8). Shows the live render target in an on-screen NSWindow.

#include "LifeCore/Output/OutputSink.h"

#include <memory>
#include <string>

namespace life {

class WindowPreviewSink : public OutputSink {
public:
    // sceneName is shown in the window title ("LifeRealtime — <sceneName>").
    // previewScale sets the window's on-screen content size relative to the
    // scene resolution (the drawable itself always renders at full scene
    // resolution; only the displayed size is scaled).
    explicit WindowPreviewSink(std::string sceneName, float previewScale = 0.5f);
    ~WindowPreviewSink() override;

    bool start(MetalContext& metal, ResourcePool& pool, uint32_t width, uint32_t height,
              std::string& outError) override;
    void publish(CommandGraph& graph, TextureHandle frame) override;
    void pump(bool& shouldQuit) override;
    void stop() override;
    const char* name() const override { return "WindowPreview"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string sceneName_;
    float previewScale_;
};

} // namespace life
