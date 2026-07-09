// LifeCore/Sim/ModuleFactory.cpp
#include "LifeCore/Sim/ModuleFactory.h"

namespace life {

ModuleFactory& ModuleFactory::instance() {
    static ModuleFactory factory;
    return factory;
}

void ModuleFactory::registerType(const std::string& typeName, Creator creator) {
    creators_[typeName] = std::move(creator);
}

std::unique_ptr<SimulationModule> ModuleFactory::create(const std::string& typeName) const {
    auto it = creators_.find(typeName);
    if (it == creators_.end()) return nullptr;
    return it->second();
}

bool ModuleFactory::hasType(const std::string& typeName) const {
    return creators_.count(typeName) > 0;
}

std::vector<std::string> ModuleFactory::registeredTypes() const {
    std::vector<std::string> names;
    names.reserve(creators_.size());
    for (auto& [name, _] : creators_) names.push_back(name);
    return names;
}

} // namespace life
