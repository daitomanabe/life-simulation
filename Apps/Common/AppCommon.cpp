// Apps/Common/AppCommon.cpp
#include "Apps/Common/AppCommon.h"

#include <mach-o/dyld.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace life::app {

std::string resolveShaderRoot(const std::string& cliValue, const char* argv0) {
    if (!cliValue.empty() && fs::exists(cliValue)) return cliValue;
    if (const char* env = std::getenv("LIFE_SHADER_ROOT");
        env && fs::exists(env))
        return env;
    if (fs::exists("Shaders")) return "Shaders";

    // Walk up from the executable (handles running from build/ trees).
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        fs::path p = fs::weakly_canonical(fs::path(buf)).parent_path();
        for (int i = 0; i < 6 && !p.empty(); ++i) {
            if (fs::exists(p / "Shaders")) return (p / "Shaders").string();
            p = p.parent_path();
        }
    }
    (void)argv0;
    return "Shaders"; // last resort; MetalContext will report the error
}

bool ensureDirectory(const std::string& path, std::string& outError) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        outError = "cannot create directory " + path + ": " + ec.message();
        return false;
    }
    return true;
}

std::string formatPassTimings(const std::vector<PassTiming>& timings, double totalMs) {
    std::string out;
    char buf[128];
    for (const auto& t : timings) {
        snprintf(buf, sizeof(buf), "%s %.2fms | ", t.label.c_str(), t.gpuMilliseconds);
        out += buf;
    }
    snprintf(buf, sizeof(buf), "gpu total %.2fms", totalMs);
    out += buf;
    return out;
}

std::string frameFilename(const std::string& dir, const std::string& stem,
                          uint32_t frameIndex, const std::string& ext) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s_%06u.%s", stem.c_str(), frameIndex, ext.c_str());
    return (fs::path(dir) / buf).string();
}

} // namespace life::app
