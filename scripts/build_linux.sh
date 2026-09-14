#!/usr/bin/env bash
# Local parity staging for the CI workflow (.github/workflows/build.yml).
# Engine core = official lsfg-vk 2.0.0 prebuilts (our engine/lsfg tree is a
# stale dev snapshot incompatible with the current Steam lsfg-vk.dll).
# Produces decky-plugin/bin/{liblsfg-vk-layer.so,liblsfg-vk-layer_32.so,lsfg-vk-cli}
set -e

LSFGVK_URL="${LSFGVK_URL:-https://builds.lsfg-vk.dev/lsfg-vk-2.0.0.tar.xz}"

echo "=== Staging official lsfg-vk 2.0.0 engine core ==="

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_BIN_DIR="${PROJECT_DIR}/decky-plugin/bin"
STAGE_DIR="${PROJECT_DIR}/build_official"

mkdir -p "${PLUGIN_BIN_DIR}" "${STAGE_DIR}"

curl -L -o "${STAGE_DIR}/lsfg-vk.tar.xz" "${LSFGVK_URL}"
tar -xJf "${STAGE_DIR}/lsfg-vk.tar.xz" -C "${STAGE_DIR}"

cp "${STAGE_DIR}/lib/liblsfg-vk-layer.so" "${PLUGIN_BIN_DIR}/liblsfg-vk-layer.so"
cp "${STAGE_DIR}/lib/liblsfg-vk-layer.x86.so" "${PLUGIN_BIN_DIR}/liblsfg-vk-layer_32.so"
cp "${STAGE_DIR}/bin/lsfg-vk-cli" "${PLUGIN_BIN_DIR}/lsfg-vk-cli"
chmod +x "${PLUGIN_BIN_DIR}/lsfg-vk-cli" || true
chmod +x "${PROJECT_DIR}/decky-plugin/skyframe-run" || true

echo "=== decky-plugin/bin ==="
ls -la "${PLUGIN_BIN_DIR}"
test -f "${PLUGIN_BIN_DIR}/liblsfg-vk-layer.so" || { echo "ERROR: 64-bit layer missing!"; exit 1; }
test -f "${PLUGIN_BIN_DIR}/liblsfg-vk-layer_32.so" || { echo "ERROR: 32-bit layer missing!"; exit 1; }
test -f "${PLUGIN_BIN_DIR}/lsfg-vk-cli" || { echo "ERROR: CLI missing!"; exit 1; }

echo "=== Official engine core staged ==="
