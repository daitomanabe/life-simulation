# LifeSimSuite — Metal life-simulation platform (design doc v1.0)
# Core: C++20 + Objective-C++ Metal bridge. Shaders compile at runtime from
# Shaders/ (CommandLineTools has no offline metal compiler; runtime compile
# also enables hot reload).
cmake_minimum_required(VERSION 3.24)
project(LifeSimSuite LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
  "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
  -Wall -Wextra -Wno-unused-parameter
)

# ---- external: miniz (C) for TinyEXR ----
add_library(miniz STATIC external/tinyexr/miniz.c)
target_include_directories(miniz PUBLIC external/tinyexr)
# miniz.c is third-party; silence its warnings.
target_compile_options(miniz PRIVATE -w)

# ---- LifeCore + built-in modules ----
add_library(lifecore STATIC
  # Metal (Objective-C++)
  LifeCore/Metal/MetalContext.mm
  LifeCore/Metal/PipelineCache.mm
  LifeCore/Metal/ResourcePool.mm
  LifeCore/Metal/GPUTimer.mm
  LifeCore/Metal/CommandGraph.mm
  # Audio
  LifeCore/Audio/OSCReceiver.cpp
  LifeCore/Audio/FeatureSmoother.cpp
  LifeCore/Audio/AudioInput.cpp
  # Params
  LifeCore/Params/ParameterBus.cpp
  LifeCore/Params/Scene.cpp
  # Sim
  LifeCore/Sim/ModuleFactory.cpp
  LifeCore/Sim/SceneRunner.cpp
  # Render
  LifeCore/Render/ColorMapPass.cpp
  LifeCore/Render/CompositePass.cpp
  LifeCore/Particle/ParticleSplatPass.cpp
  # Spatial
  LifeCore/Spatial/SpatialHashGrid.cpp
  # IO
  LifeCore/IO/ImageWriter.mm
  LifeCore/IO/FrameRecorder.cpp
  LifeCore/IO/CaptureReplay.cpp
  # Modules
  Modules/FieldModules/ReactionDiffusion.cpp
  Modules/FieldModules/Lenia.cpp
  Modules/FieldModules/CellularAutomata.cpp
  Modules/FieldModules/AudioToField.cpp
  Modules/ParticleModules/SlimeMold.cpp
  Modules/ParticleModules/ParticleLife.cpp
  Modules/ParticleModules/Boids.cpp
  Modules/RegisterModules.cpp
)

target_include_directories(lifecore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/external
)

target_link_libraries(lifecore PUBLIC
  miniz
  "-framework Metal"
  "-framework Foundation"
  "-framework CoreGraphics"
  "-framework ImageIO"
  "-framework CoreFoundation"
)

# nlohmann/json exceptions are caught locally; keep exceptions enabled.

# ---- Apps (thin runners; all simulation lives in lifecore) ----
add_library(appcommon STATIC Apps/Common/AppCommon.cpp)
target_link_libraries(appcommon PUBLIC lifecore)

add_executable(LifeRealtime Apps/LifeRealtime/main.cpp)
target_link_libraries(LifeRealtime PRIVATE appcommon)

add_executable(LifeOfflineRender Apps/LifeOfflineRender/main.cpp)
target_link_libraries(LifeOfflineRender PRIVATE appcommon)

add_executable(LifeBench Apps/LifeBench/main.cpp)
target_link_libraries(LifeBench PRIVATE appcommon)
