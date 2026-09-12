#!/usr/bin/env bash
set -e

echo "=== Building SkyFrame Vulkan Layer for Linux x86_64 ==="

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
PLUGIN_BIN_DIR="${PROJECT_DIR}/decky-plugin/bin"

mkdir -p "${BUILD_DIR}"
mkdir -p "${PLUGIN_BIN_DIR}"
mkdir -p "${PLUGIN_BIN_DIR}/models"

# 1. Compile Vulkan Shaders and C++ Layer
cd "${BUILD_DIR}"
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)

# 2. Copy artifacts to Decky Plugin bin directory
cp "${BUILD_DIR}/libVkLayer_skyframe.so" "${PLUGIN_BIN_DIR}/"
cp "${BUILD_DIR}/vk_layer_skyframe.json" "${PLUGIN_BIN_DIR}/"
cp -r "${PROJECT_DIR}/models/"* "${PLUGIN_BIN_DIR}/models/"

echo "=== SkyFrame Vulkan Layer successfully built and copied to decky-plugin/bin/ ==="
