cmake_minimum_required(VERSION 3.16)
project(SkyFrame VERSION 1.9.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# LSFG Neural Engine (Lossless Scaling Vulkan layer and CLI)
add_subdirectory(engine/lsfg)
