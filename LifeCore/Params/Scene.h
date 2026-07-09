#pragma once
// LifeCore/Params/Scene.h
// Scene description loaded from JSON (design doc §14.1): seed, resolution,
// module list with params, audio mappings, and (from Phase 5 on) module
// connections. Parsed with nlohmann/json; params stay as raw JSON so each
// module interprets its own block.

#include "LifeCore/Params/ParameterBus.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace life {

struct ModuleSpec {
    std::string type;   // "ReactionDiffusion", "Lenia", ...
    std::string name;   // instance name, e.g. "rd0"
    bool enabled = true;
    nlohmann::json params; // module-specific parameter block
};

struct ConnectionSpec {
    std::string from; // "lenia0.field"
    std::string to;   // "slime0.attractorField"
    std::string mode; // "sample", ...
};

struct Scene {
    uint32_t seed = 1234;
    uint32_t width = 1920;
    uint32_t height = 1080;
    std::vector<ModuleSpec> modules;
    std::vector<AudioMappingSpec> audioMappings;
    std::vector<ConnectionSpec> connections;
    nlohmann::json raw; // full document (for metadata snapshots)

    static std::optional<Scene> loadFile(const std::string& path, std::string& outError);
    static std::optional<Scene> parse(const nlohmann::json& j, std::string& outError);

    // Push module params + defaults into a ParameterBus as
    // "<moduleName>.<param>" base values (numeric leaves only).
    void applyBaseParams(ParameterBus& bus) const;
};

} // namespace life
