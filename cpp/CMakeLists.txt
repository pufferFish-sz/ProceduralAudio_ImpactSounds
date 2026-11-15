cmake_minimum_required(VERSION 3.20)
project(procaudio LANGUAGES CXX)

# procaudio-core: engine-agnostic C++ port of the Python prototype in
# proto/ (impact, sliding model C, rolling). See proto/docs/cpp_plugin_design.md.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  add_compile_options(/W4 /permissive- /fp:precise /utf-8)
  add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(procaudio_core STATIC
  core/src/core.cpp
  core/src/dsp.cpp
  core/src/profile.cpp
  core/src/bodies.cpp
  core/src/impact.cpp
  core/src/rolling.cpp
  core/src/sliding.cpp
  core/src/voice.cpp
  core/src/catalog.cpp
  core/src/body_desc.cpp
  core/src/scenes.cpp)
target_include_directories(procaudio_core PUBLIC core/include)

add_executable(procaudio_render tools/render.cpp)
target_link_libraries(procaudio_render PRIVATE procaudio_core)

enable_testing()
add_executable(procaudio_tests tests/test_core.cpp)
target_link_libraries(procaudio_tests PRIVATE procaudio_core)
add_test(NAME core COMMAND procaudio_tests)

# Unity native plug-in (C API), copied into the Unity demo project.
add_subdirectory(plugins/unity)

# FMOD sound module: built only when the FMOD Engine SDK is found.
# Install the FMOD Engine (2.02.x, to match FMOD Studio 2.02.23) and set
# FMOD_SDK_DIR to its "api" folder's parent, e.g.
#   C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows
set(FMOD_SDK_DIR "$ENV{FMOD_SDK_DIR}" CACHE PATH "FMOD Engine SDK root")
if(FMOD_SDK_DIR AND EXISTS "${FMOD_SDK_DIR}/api/core/inc/fmod_dsp.h")
  add_subdirectory(plugins/fmod)
else()
  message(STATUS "FMOD SDK not found (FMOD_SDK_DIR): skipping the FMOD plugin")
endif()
