#!/usr/bin/env bash
set -e

echo "=== Building SkyFrame LSFG Vulkan Layer for Linux ==="

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
PLUGIN_BIN_DIR="${PROJECT_DIR}/decky-plugin/bin"

mkdir -p "${BUILD_DIR}"
mkdir -p "${PLUGIN_BIN_DIR}"

# 1. Compile Vulkan Layer & CLI
cd "${BUILD_DIR}"
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)

# 2. Copy artifacts to Decky Plugin bin directory
find . -name "liblsfg-vk-layer.so" -exec cp {} "${PLUGIN_BIN_DIR}/liblsfg-vk-layer.so" \;
find . -name "VkLayer_LSFGVK_frame_generation.json" -exec cp {} "${PLUGIN_BIN_DIR}/" \;
find . -name "lsfg-vk-cli" -type f -exec cp {} "${PLUGIN_BIN_DIR}/lsfg-vk-cli" \;
chmod +x "${PLUGIN_BIN_DIR}/lsfg-vk-cli" || true

echo "=== SkyFrame LSFG Vulkan Layer successfully built and copied to decky-plugin/bin/ ==="
