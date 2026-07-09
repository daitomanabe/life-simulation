#pragma once
// LifeCore/Sim/ModuleFactory.h
// Maps scene "type" strings to module constructors. Registration is explicit
// (registerBuiltinModules() in Modules/) to avoid static-init order issues.

#include "LifeCore/Sim/SimulationModule.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace life {

class ModuleFactory {
public:
    using Creator = std::function<std::unique_ptr<SimulationModule>()>;

    static ModuleFactory& instance();

    void registerType(const std::string& typeName, Creator creator);
    std::unique_ptr<SimulationModule> create(const std::string& typeName) const;
    bool hasType(const std::string& typeName) const;
    std::vector<std::string> registeredTypes() const;

private:
    std::unordered_map<std::string, Creator> creators_;
};

} // namespace life

// Implemented in Modules/RegisterModules.cpp — call once at app startup.
namespace life::modules {
void registerBuiltinModules();
}
