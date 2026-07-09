#pragma once
// LifeCore/Params/ParameterBus.h
// Central parameter table. Flow (design doc §14.2):
//   OSC → AudioFeatureState → AudioMapping → ParameterBus → SimulationUniforms
// Simulations never see OSC addresses; they read final values by key
// ("moduleName.paramName"). A mapping's contribution is added on top of the
// scene's base value each frame.

#include "LifeCore/Audio/AudioFeatureState.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace life {

struct AudioMappingSpec {
    std::string source;  // "kick", "kick.trigger", "rms", "fft.low", ...
    std::string target;  // "rd0.feed"
    float scale = 1.0f;
    float offset = 0.0f;
    float smoothing = 0.0f; // seconds to ~63% (0 = immediate)
};

class ParameterBus {
public:
    void clear();

    // Base values come from the scene JSON.
    void setBase(const std::string& key, float value);
    float base(const std::string& key, float fallback = 0.0f) const;

    void addMapping(const AudioMappingSpec& spec);
    const std::vector<AudioMappingSpec>& mappings() const { return mappings_; }

    // Apply mappings for this frame.
    void update(const AudioFeatureState& audio, float dt);

    // Final value = base + Σ mapped contributions (+ manual override).
    float value(const std::string& key, float fallback = 0.0f) const;

    // Manual override layer (future GUI / control OSC).
    void setOverride(const std::string& key, float value);
    void clearOverride(const std::string& key);

    // Resolve an audio feature by name ("kick", "kick.trigger", "low", ...).
    static float featureValue(const AudioFeatureState& s, const std::string& name);

private:
    struct MappingState {
        AudioMappingSpec spec;
        float smoothedValue = 0.0f;
    };

    std::unordered_map<std::string, float> base_;
    std::unordered_map<std::string, float> audioContribution_;
    std::unordered_map<std::string, float> overrides_;
    std::vector<AudioMappingSpec> mappings_;
    std::vector<MappingState> mappingStates_;
};

} // namespace life
