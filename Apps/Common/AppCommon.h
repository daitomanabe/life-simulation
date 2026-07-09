#pragma once
// Apps/Common/AppCommon.h
// Small shared helpers for the runner apps (path resolution, status lines).
// Apps stay thin: pacing + IO policy only; all simulation goes through
// SceneRunner (design doc §22-10).

#include "LifeCore/Metal/GPUTimer.h"

#include <string>
#include <vector>

namespace life::app {

// Resolve the Shaders/ directory: explicit CLI value → $LIFE_SHADER_ROOT →
// ./Shaders → walk up from the executable location.
std::string resolveShaderRoot(const std::string& cliValue, const char* argv0);

// Ensure a directory exists (mkdir -p).
bool ensureDirectory(const std::string& path, std::string& outError);

// "pass_a 0.12ms | pass_b 0.34ms | total 0.61ms"
std::string formatPassTimings(const std::vector<PassTiming>& timings, double totalMs);

std::string frameFilename(const std::string& dir, const std::string& stem,
                          uint32_t frameIndex, const std::string& ext);

} // namespace life::app
