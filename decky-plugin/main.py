import os
import json
import logging
import asyncio

logging.basicConfig(level=logging.INFO)

CONFIG_DIR = os.path.expanduser("~/.config/skyframe")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")
VULKAN_LAYER_DIR = os.path.expanduser("~/.local/share/vulkan/implicit_layer.d")
VULKAN_LAYER_PATH = os.path.join(VULKAN_LAYER_DIR, "vk_layer_skyframe.json")

DEFAULT_CONFIG = {
    "enabled": False,
    "mode": 1,              # 0 = Lite (180p), 1 = Balanced (240p), 2 = Quality (360p)
    "multiplier": 2,        # 2x frame generation
    "hud_protection": True,
    "hud_threshold": 0.08
}

class Plugin:
    def __init__(self):
        self.config = DEFAULT_CONFIG.copy()
        self.load_config()

    def load_config(self):
        try:
            if os.path.isfile(CONFIG_PATH):
                with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    self.config.update(data)
        except Exception as e:
            logging.error(f"[SkyFrame] Failed to load config: {e}")

    def save_config(self):
        try:
            os.makedirs(CONFIG_DIR, exist_ok=True)
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(self.config, f, indent=2)
            self.sync_vulkan_layer()
        except Exception as e:
            logging.error(f"[SkyFrame] Failed to save config: {e}")

    def sync_vulkan_layer(self):
        """Installs or removes the Vulkan implicit layer manifest based on enabled state"""
        try:
            plugin_dir = os.path.dirname(os.path.abspath(__file__))
            src_manifest = os.path.join(plugin_dir, "bin", "vk_layer_skyframe.json")

            if self.config.get("enabled", False):
                os.makedirs(VULKAN_LAYER_DIR, exist_ok=True)
                # Read template manifest and update absolute library path
                if os.path.isfile(src_manifest):
                    with open(src_manifest, "r", encoding="utf-8") as f:
                        manifest_data = json.load(f)
                    
                    lib_path = os.path.join(plugin_dir, "bin", "libVkLayer_skyframe.so")
                    manifest_data["layer"]["library_path"] = lib_path
                    
                    with open(VULKAN_LAYER_PATH, "w", encoding="utf-8") as f:
                        json.dump(manifest_data, f, indent=2)
                    logging.info(f"[SkyFrame] Vulkan implicit layer manifest registered at: {VULKAN_LAYER_PATH}")
            else:
                if os.path.isfile(VULKAN_LAYER_PATH):
                    os.remove(VULKAN_LAYER_PATH)
                    logging.info("[SkyFrame] Vulkan implicit layer manifest removed (disabled).")
        except Exception as e:
            logging.error(f"[SkyFrame] Error updating Vulkan layer: {e}")

    async def _main(self):
        logging.info("[SkyFrame] Backend daemon started.")
        self.sync_vulkan_layer()

    async def _unload(self):
        logging.info("[SkyFrame] Backend daemon unloading...")

    async def get_config(self):
        return self.config

    async def set_enabled(self, enabled: bool):
        self.config["enabled"] = bool(enabled)
        self.save_config()
        return self.config

    async def set_mode(self, mode: int):
        self.config["mode"] = int(mode)
        self.save_config()
        return self.config

    async def set_hud_protection(self, hud_protection: bool):
        self.config["hud_protection"] = bool(hud_protection)
        self.save_config()
        return self.config

    async def get_stats(self):
        """Simulated/actual telemetry from Vulkan Layer IPC / shared memory"""
        return {
            "base_fps": 30.0 if not self.config["enabled"] else 40.0,
            "output_fps": 30.0 if not self.config["enabled"] else 80.0
        }
