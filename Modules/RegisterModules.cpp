// Modules/RegisterModules.cpp
// Explicit registration of all built-in simulation modules. Called once at
// app startup (avoids static-initializer ordering games).
#include "LifeCore/Sim/ModuleFactory.h"

#include "Modules/FieldModules/AudioToField.h"
#include "Modules/FieldModules/CellularAutomata.h"
#include "Modules/FieldModules/Fluid.h"
#include "Modules/FieldModules/Lenia.h"
#include "Modules/FieldModules/PresenceField.h"
#include "Modules/FieldModules/ReactionDiffusion.h"
#include "Modules/ParticleModules/Boids.h"
#include "Modules/ParticleModules/ParticleLife.h"
#include "Modules/ParticleModules/SlimeMold.h"
#include "Modules/ParticleModules/Tracers.h"

namespace life::modules {

void registerBuiltinModules() {
    auto& f = ModuleFactory::instance();
    f.registerType("ReactionDiffusion",
                   [] { return std::make_unique<ReactionDiffusionModule>(); });
    f.registerType("Lenia", [] { return std::make_unique<LeniaModule>(); });
    f.registerType("CellularAutomata",
                   [] { return std::make_unique<CellularAutomataModule>(); });
    f.registerType("AudioToField",
                   [] { return std::make_unique<AudioToFieldModule>(); });
    f.registerType("PresenceField",
                   [] { return std::make_unique<PresenceFieldModule>(); });
    f.registerType("SlimeMold", [] { return std::make_unique<SlimeMoldModule>(); });
    f.registerType("ParticleLife",
                   [] { return std::make_unique<ParticleLifeModule>(); });
    f.registerType("Boids", [] { return std::make_unique<BoidsModule>(); });
    f.registerType("Fluid", [] { return std::make_unique<FluidModule>(); });
    f.registerType("Tracers", [] { return std::make_unique<TracersModule>(); });
}

} // namespace life::modules
