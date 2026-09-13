cmake_minimum_required(VERSION 3.16)
project(SkyFrame VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# LSFG Neural Engine (Lossless Scaling Vulkan layer and CLI)
add_subdirectory(engine/lsfg)

# SkyFrame own layer identity (phase-1: alias manifests over lsfg-vk 2.0 core)
set(SKYFRAME_LAYER_LIBRARY_PATH liblsfg-vk-layer.so CACHE STRING "SkyFrame layer library path")
add_subdirectory(engine/skyframe)
