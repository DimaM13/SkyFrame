#!/usr/bin/env bash
set -e

echo "=== SkyFrame Decky Plugin Installer ==="

PLUGINS_DIR="/home/deck/homebrew/plugins"
ZIP_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/SkyFrame.zip"

if [ ! -f "$ZIP_FILE" ]; then
    echo "Error: SkyFrame.zip not found! Run scripts/package_decky_zip.py first."
    exit 1
fi

if [ ! -d "$PLUGINS_DIR" ]; then
    echo "Warning: Decky plugins directory ($PLUGINS_DIR) not found. Creating..."
    sudo mkdir -p "$PLUGINS_DIR"
    sudo chown -R deck:deck "$PLUGINS_DIR"
fi

echo "Extracting SkyFrame to ${PLUGINS_DIR}..."
unzip -o "$ZIP_FILE" -d "$PLUGINS_DIR/"

echo "Restarting Decky Loader plugin service..."
sudo systemctl restart plugin_loader.service || true

echo "=== SkyFrame successfully installed! Open Quick Access Menu (...) to configure. ==="
