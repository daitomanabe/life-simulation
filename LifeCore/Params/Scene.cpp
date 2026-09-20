// LifeCore/Params/Scene.cpp
#include "LifeCore/Params/Scene.h"

#include <fstream>

namespace life {

std::optional<Scene> Scene::loadFile(const std::string& path, std::string& outError) {
    std::ifstream f(path);
    if (!f) {
        outError = "cannot open scene file: " + path;
        return std::nullopt;
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        outError = std::string("scene JSON parse error: ") + e.what();
        return std::nullopt;
    }
    return parse(j, outError);
}

std::optional<Scene> Scene::parse(const nlohmann::json& j, std::string& outError) {
    Scene scene;
    scene.raw = j;
    try {
        scene.seed = j.value("seed", 1234u);
        if (j.contains("resolution") && j["resolution"].is_array() &&
            j["resolution"].size() == 2) {
            scene.width = j["resolution"][0].get<uint32_t>();
            scene.height = j["resolution"][1].get<uint32_t>();
        }
        for (const auto& m : j.value("modules", nlohmann::json::array())) {
            ModuleSpec spec;
            spec.type = m.value("type", "");
            spec.name = m.value("name", spec.type);
            spec.enabled = m.value("enabled", true);
            spec.params = m.value("params", nlohmann::json::object());
            if (spec.type.empty()) {
                outError = "module entry missing \"type\"";
                return std::nullopt;
            }
            scene.modules.push_back(std::move(spec));
        }
        // audioMappings・musicMappings・automation は同じ形。3つに分けてある
        // のは意図の宣言のためで（音リアクティブ／曲の構成／時間駆動）、どれに
        // 書いても同じ経路を通る（ParameterBus が source 名だけで音・レーン・
        // セクション・イベント・時間を見分ける）。
        auto readMappings = [&](const char* key) -> bool {
            for (const auto& a : j.value(key, nlohmann::json::array())) {
                AudioMappingSpec spec;
                spec.source = a.value("source", "");
                spec.target = a.value("target", "");
                spec.inMin = a.value("inMin", 0.0f);
                spec.inMax = a.value("inMax", 0.0f);
                spec.scale = a.value("scale", 1.0f);
                spec.offset = a.value("offset", 0.0f);
                spec.smoothing = a.value("smoothing", 0.0f);
                spec.mode = mappingModeFromString(a.value("mode", "add"));
                if (spec.source.empty() || spec.target.empty()) {
                    outError = std::string(key) + " entry missing source/target";
                    return false;
                }
                // "time.*" ソースはここで検証する。壊れた／0 以下の period を
                // 読み込み時に弾く — 黙って 0 を出す automation は、8 時間
                // ノンストップで動くこの手のシーンでは気づかれない。
                TimeSourceKind timeKind;
                float timePeriod, timePhase;
                std::string timeErr;
                if (!parseTimeSource(spec.source, timeKind, timePeriod, timePhase, timeErr)) {
                    outError = std::string(key) + ": " + timeErr;
                    return false;
                }
                scene.audioMappings.push_back(std::move(spec));
            }
            return true;
        };
        if (!readMappings("audioMappings")) return std::nullopt;
        if (!readMappings("musicMappings")) return std::nullopt;
        if (!readMappings("automation")) return std::nullopt;
        for (const auto& c : j.value("connections", nlohmann::json::array())) {
            ConnectionSpec spec;
            spec.from = c.value("from", "");
            spec.to = c.value("to", "");
            spec.mode = c.value("mode", "sample");
            scene.connections.push_back(std::move(spec));
        }
    } catch (const std::exception& e) {
        outError = std::string("scene JSON structure error: ") + e.what();
        return std::nullopt;
    }
    return scene;
}

void Scene::applyBaseParams(ParameterBus& bus) const {
    // time.noise マッピングが種を派生させるのに要る。addMapping() より前に
    // 設定すること。
    bus.setSeed(seed);
    for (const auto& m : modules) {
        for (auto it = m.params.begin(); it != m.params.end(); ++it) {
            if (it.value().is_number()) {
                bus.setBase(m.name + "." + it.key(), it.value().get<float>());
            } else if (it.value().is_boolean()) {
                bus.setBase(m.name + "." + it.key(),
                            it.value().get<bool>() ? 1.0f : 0.0f);
            }
            // Arrays/objects (palettes etc.) are consumed by the module
            // directly from its ModuleSpec.params.
        }
    }
    for (const auto& a : audioMappings) bus.addMapping(a);
}

} // namespace life
