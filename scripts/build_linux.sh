#!/usr/bin/env bash
# Local parity build for the CI workflow (.github/workflows/build.yml).
# Produces decky-plugin/bin/{liblsfg-vk-layer.so,liblsfg-vk-layer_32.so,lsfg-vk-cli}
set -e

echo "=== Building SkyFrame LSFG Vulkan Layer for Linux ==="

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_BIN_DIR="${PROJECT_DIR}/decky-plugin/bin"

mkdir -p "${PLUGIN_BIN_DIR}"

# 1. 64-bit build (layer + CLI)
BUILD64="${PROJECT_DIR}/build64"
mkdir -p "${BUILD64}"
cd "${BUILD64}"
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)
cd "${PROJECT_DIR}"

# 2. 32-bit build (layer only, needs gcc/g++-multilib + libvulkan-dev:i386)
BUILD32="${PROJECT_DIR}/build32"
mkdir -p "${BUILD32}"
cd "${BUILD32}"
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-m32" -DCMAKE_CXX_FLAGS="-m32" -DCMAKE_SHARED_LINKER_FLAGS="-m32" -DLSFGVK_BUILD_CLI=OFF
cmake --build . --config Release -j$(nproc)
cd "${PROJECT_DIR}"

# 3. Stage artifacts into decky-plugin/bin/
find "${BUILD64}" -name "liblsfg-vk-layer.so" -exec cp {} "${PLUGIN_BIN_DIR}/liblsfg-vk-layer.so" \;
find "${BUILD32}" -name "liblsfg-vk-layer.so" -exec cp {} "${PLUGIN_BIN_DIR}/liblsfg-vk-layer_32.so" \;
find "${BUILD64}" -name "lsfg-vk-cli" -type f -exec cp {} "${PLUGIN_BIN_DIR}/lsfg-vk-cli" \;
chmod +x "${PLUGIN_BIN_DIR}/lsfg-vk-cli" || true
chmod +x "${PROJECT_DIR}/decky-plugin/skyframe-run" || true

echo "=== decky-plugin/bin ==="
ls -la "${PLUGIN_BIN_DIR}"
test -f "${PLUGIN_BIN_DIR}/liblsfg-vk-layer.so" || { echo "ERROR: 64-bit layer missing!"; exit 1; }
test -f "${PLUGIN_BIN_DIR}/liblsfg-vk-layer_32.so" || { echo "ERROR: 32-bit layer missing!"; exit 1; }
test -f "${PLUGIN_BIN_DIR}/lsfg-vk-cli" || { echo "ERROR: CLI missing!"; exit 1; }

echo "=== SkyFrame LSFG Vulkan Layer successfully built and staged ==="
