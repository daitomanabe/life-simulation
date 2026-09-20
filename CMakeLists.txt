# LifeSimSuite — Metal life-simulation platform (design doc v1.0)
# Core: C++20 + Objective-C++ Metal bridge. Shaders compile at runtime from
# Shaders/ (CommandLineTools has no offline metal compiler; runtime compile
# also enables hot reload).
cmake_minimum_required(VERSION 3.24)
# OBJC (plain Objective-C, .m) is needed alongside OBJCXX (Objective-C++,
# .mm) starting phase 6: the vendored Syphon-Framework sources are .m.
project(LifeSimSuite LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Phase 6 (docs/specs/phase6_output_sinks.md): SyphonSink + SyphonCheck vendor
# Syphon-Framework (external/Syphon) as plain static source. Default ON —
# flip OFF to build LifeRealtime/LifeOfflineRender/LifeBench without it.
option(LIFE_WITH_SYPHON "Build SyphonSink output + SyphonCheck verifier" ON)

add_compile_options(
  "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
  -Wall -Wextra -Wno-unused-parameter
)

# ---- external: miniz (C) for TinyEXR ----
add_library(miniz STATIC external/tinyexr/miniz.c)
target_include_directories(miniz PUBLIC external/tinyexr)
# miniz.c is third-party; silence its warnings.
target_compile_options(miniz PRIVATE -w)

# ---- external: Syphon-Framework (vendored source, Metal server/client path
# only — see external/Syphon and docs/specs/phase6_output_sinks.md §5/§8-5).
# OpenGL server/client, the deprecated SyphonServer/SyphonClient umbrella
# wrappers, and SyphonIOSurfaceImage{Core,Legacy} (OpenGL-only) are excluded;
# only the .m files actually reachable from SyphonMetalServer/SyphonMetalClient
# by following #import are compiled. ----
if(LIFE_WITH_SYPHON)
  set(SYPHON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/Syphon)
  set(SYPHON_SOURCES
    # messaging (CFMessagePort, GCD-driven — shared by server + client)
    ${SYPHON_DIR}/SyphonPrivate.m
    ${SYPHON_DIR}/SyphonMessaging.m
    ${SYPHON_DIR}/SyphonMessageReceiver.m
    ${SYPHON_DIR}/SyphonMessageSender.m
    ${SYPHON_DIR}/SyphonMessageQueue.m
    ${SYPHON_DIR}/SyphonCFMessageReceiver.m
    ${SYPHON_DIR}/SyphonCFMessageSender.m
    ${SYPHON_DIR}/SyphonDispatch.c
    # server (Metal)
    ${SYPHON_DIR}/SyphonServerBase.m
    ${SYPHON_DIR}/SyphonServerConnectionManager.m
    ${SYPHON_DIR}/SyphonServerRendererMetal.m
    ${SYPHON_DIR}/SyphonMetalServer.m
    # discovery (server + client)
    ${SYPHON_DIR}/SyphonServerDirectory.m
    # client (Metal) — only used by Apps/SyphonCheck, still built here so
    # both sides live in one vendored static lib.
    ${SYPHON_DIR}/SyphonClientConnectionManager.m
    ${SYPHON_DIR}/SyphonClientBase.m
    ${SYPHON_DIR}/SyphonMetalClient.m
  )
  add_library(syphon_vendor STATIC ${SYPHON_SOURCES})
  target_include_directories(syphon_vendor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external)
  # Third-party code: silence its warnings, and force-include the Xcode
  # prefix header it was written to always have (defines SYPHONLOG; see
  # external/Syphon/Syphon_Prefix.pch — Xcode injects this automatically via
  # GCC_PREFIX_HEADER, plain CMake does not).
  target_compile_options(syphon_vendor PRIVATE
    -w
    -include ${SYPHON_DIR}/Syphon_Prefix.pch
  )
  # NOTE (deviation from the phase 6 spec, which assumed non-ARC Syphon
  # sources — see final report / §5 "Syphon の vendor で実際に必要だった...
  # define"): this upstream snapshot's Metal-era files (2020-2023,
  # SyphonMetalServer/Client, the SyphonCF*/SyphonMessage* messaging stack,
  # SyphonServerBase/ConnectionManager/Directory, ...) are ARC code —
  # SyphonCFMessageSender.m uses `__weak`, which is a hard compile error
  # under -fno-objc-arc ("cannot create __weak reference in file using
  # manual reference counting"), several files use __bridge casts, and the
  # vendored Syphon.xcodeproj itself sets CLANG_ENABLE_OBJC_ARC = YES. None
  # of the chosen files call retain/release/autorelease or [super dealloc]
  # (the MRC tell), so compiling this whole set under ARC — like the rest
  # of this project's OBJCXX — is correct and matches upstream's own build
  # settings, not a workaround. (SyphonDispatch.c is plain C; ARC does not
  # apply to it either way.)
  set(SYPHON_OBJC_SOURCES ${SYPHON_SOURCES})
  list(REMOVE_ITEM SYPHON_OBJC_SOURCES ${SYPHON_DIR}/SyphonDispatch.c)
  set_source_files_properties(${SYPHON_OBJC_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc"
  )
  target_link_libraries(syphon_vendor PUBLIC
    "-framework Foundation"
    "-framework Metal"
    "-framework IOSurface"
    "-framework Cocoa"
    "-framework CoreFoundation"
  )
endif()

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
  # Music (events JSON → 楽曲構造。Audio が「強さ」、こちらが「意味」)
  LifeCore/Music/MusicTimeline.cpp
  # Params
  LifeCore/Params/ParameterBus.cpp
  LifeCore/Params/Scene.cpp
  # Sim
  LifeCore/Sim/ModuleFactory.cpp
  LifeCore/Sim/SceneRunner.cpp
  # Render
  LifeCore/Render/ColorMapPass.cpp
  LifeCore/Render/CompositePass.cpp
  LifeCore/Render/PostPass.cpp
  LifeCore/Particle/ParticleSplatPass.cpp
  # Spatial
  LifeCore/Spatial/SpatialHashGrid.cpp
  # Output (SyphonSink.mm is appended below, conditionally on LIFE_WITH_SYPHON)
  LifeCore/Output/WindowPreviewSink.mm
  # IO
  LifeCore/IO/ImageWriter.mm
  LifeCore/IO/FrameRecorder.cpp
  LifeCore/IO/NpyWriter.cpp
  LifeCore/IO/CaptureReplay.cpp
  LifeCore/IO/MovieWriter.mm
  # Modules
  Modules/FieldModules/ReactionDiffusion.cpp
  Modules/FieldModules/Lenia.cpp
  Modules/FieldModules/CellularAutomata.cpp
  Modules/FieldModules/AudioToField.cpp
  Modules/FieldModules/Fluid.cpp
  Modules/ParticleModules/SlimeMold.cpp
  Modules/ParticleModules/ParticleLife.cpp
  Modules/ParticleModules/Boids.cpp
  Modules/ParticleModules/Tracers.cpp
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
  # WindowPreviewSink (LifeCore/Output) — always built, needs AppKit for
  # NSWindow/NSApplication and QuartzCore for CAMetalLayer.
  "-framework AppKit"
  "-framework QuartzCore"
  # Phase 10 (docs/specs/phase10_movie_writer.md): MovieWriter.mm direct
  # ProRes/HEVC/H264 .mov export via AVAssetWriter.
  "-framework AVFoundation"
  "-framework CoreMedia"
  "-framework CoreVideo"
  "-framework VideoToolbox"
)

if(LIFE_WITH_SYPHON)
  target_sources(lifecore PRIVATE LifeCore/Output/SyphonSink.mm)
  target_compile_definitions(lifecore PUBLIC LIFE_WITH_SYPHON=1)
  target_link_libraries(lifecore PUBLIC syphon_vendor)
endif()

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

# Phase 7 (docs/specs/phase7_presetlab.md): parameter/seed sweep runner.
add_executable(LifePresetLab Apps/LifePresetLab/main.cpp)
target_link_libraries(LifePresetLab PRIVATE appcommon)

# SyphonCheck (phase 6 §6): standalone verification client, deliberately not
# linked against lifecore/appcommon — it only exercises the vendored Syphon
# client path from a separate process.
if(LIFE_WITH_SYPHON)
  add_executable(SyphonCheck Apps/SyphonCheck/main.mm)
  target_link_libraries(SyphonCheck PRIVATE syphon_vendor)
endif()
