#pragma once
// LifeCore/Output/SyphonSink.h
// Pure C++ facade — Metal/Syphon types stay inside SyphonSink.mm (design doc
// §3.8). Publishes the live render target as a Syphon (Metal) server so
// apps like Resolume/MadMapper can pick it up. Only compiled when
// LIFE_WITH_SYPHON is enabled (see CMakeLists.txt / external/Syphon).

#include "LifeCore/Output/OutputSink.h"

#include <memory>
#include <string>

namespace life {

class SyphonSink : public OutputSink {
public:
    // serverName is the human-readable Syphon server name clients discover
    // (SyphonServerDirectory / Resolume's source list, ...).
    explicit SyphonSink(std::string serverName);
    ~SyphonSink() override;

    bool start(MetalContext& metal, ResourcePool& pool, uint32_t width, uint32_t height,
              std::string& outError) override;
    void publish(CommandGraph& graph, TextureHandle frame) override;
    void pump(bool& shouldQuit) override;
    void stop() override;
    const char* name() const override { return "Syphon"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string serverName_;
};

} // namespace life
