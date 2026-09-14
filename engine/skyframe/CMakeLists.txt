cmake_minimum_required(VERSION 3.16)

# Standalone-capable: CI builds this dir directly
#   cmake -S engine/skyframe -B build-shim -DCMAKE_BUILD_TYPE=Release
# and it also works via add_subdirectory() from the repo root.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(SkyFrameShim LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

find_package(Vulkan QUIET)

add_library(skyframe-shim SHARED layer/skyframe_shim.cpp)

target_include_directories(skyframe-shim PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_options(skyframe-shim PRIVATE -Wall -Wextra -Wno-unknown-warning-option)
set_target_properties(skyframe-shim PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "skyframe-shim")

# Phase-1 alias manifests (point at lsfg-vk 2.0 core) are generated at
# runtime by decky-plugin/main.py. Phase-2 (forked libskyframe.so with
# live-toggle/adaptive/scheduler) will extend this target, see PACING.md.
