import os
import json
import logging
import shutil
import asyncio

logging.basicConfig(level=logging.INFO)

HOME = os.path.expanduser("~")
CONFIG_DIR = os.path.join(HOME, ".config", "skyframe")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")
LOCAL_LIB_DIR = os.path.join(HOME, ".local", "lib")
VULKAN_LAYER_DIR = os.path.join(HOME, ".local", "share", "vulkan", "implicit_layer.d")
VULKAN_LAYER_64_PATH = os.path.join(VULKAN_LAYER_DIR, "vk_layer_skyframe.json")
VULKAN_LAYER_32_PATH = os.path.join(VULKAN_LAYER_DIR, "vk_layer_skyframe_32.json")
MODELS_DIR = os.path.join(HOME, ".local", "share", "skyframe", "models")

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
        """Installs layer libraries into ~/.local/lib and registers implicit layer manifests"""
        try:
            plugin_dir = os.path.dirname(os.path.abspath(__file__))
            bin_dir = os.path.join(plugin_dir, "bin")

            os.makedirs(LOCAL_LIB_DIR, exist_ok=True)
            os.makedirs(VULKAN_LAYER_DIR, exist_ok=True)
            os.makedirs(MODELS_DIR, exist_ok=True)

            # Copy models to standard ~/.local/share/skyframe/models
            models_src = os.path.join(bin_dir, "models")
            if os.path.isdir(models_src):
                for mf in os.listdir(models_src):
                    src_f = os.path.join(models_src, mf)
                    dst_f = os.path.join(MODELS_DIR, mf)
                    if os.path.isfile(src_f) and not os.path.isfile(dst_f):
                        shutil.copy2(src_f, dst_f)

            # Copy 64-bit library
            lib64_src = os.path.join(bin_dir, "libVkLayer_skyframe.so")
            lib64_dst = os.path.join(LOCAL_LIB_DIR, "libVkLayer_skyframe.so")
            if os.path.isfile(lib64_src):
                shutil.copy2(lib64_src, lib64_dst)
                os.chmod(lib64_dst, 0o755)

            # Copy 32-bit library (if present)
            lib32_src = os.path.join(bin_dir, "libVkLayer_skyframe_32.so")
            lib32_dst = os.path.join(LOCAL_LIB_DIR, "libVkLayer_skyframe_32.so")
            if os.path.isfile(lib32_src):
                shutil.copy2(lib32_src, lib32_dst)
                os.chmod(lib32_dst, 0o755)

            if self.config.get("enabled", False):
                # 64-bit implicit manifest
                manifest_64 = {
                    "file_format_version": "1.0.0",
                    "layer": {
                        "name": "VK_LAYER_SKYFRAME_framegen",
                        "type": "GLOBAL",
                        "library_path": "../../../lib/libVkLayer_skyframe.so",
                        "api_version": "1.3.0",
                        "implementation_version": "1",
                        "description": "SkyFrame Native AI Frame Generation Layer (64-bit)",
                        "functions": {
                            "vkGetInstanceProcAddr": "skyframe_GetInstanceProcAddr",
                            "vkGetDeviceProcAddr": "skyframe_GetDeviceProcAddr"
                        },
                        "disable_environment": {
                            "DISABLE_SKYFRAME": "1"
                        }
                    }
                }
                with open(VULKAN_LAYER_64_PATH, "w", encoding="utf-8") as f:
                    json.dump(manifest_64, f, indent=2)
                logging.info(f"[SkyFrame] Registered 64-bit Vulkan layer at {VULKAN_LAYER_64_PATH}")

                # 32-bit implicit manifest (if 32-bit library exists)
                if os.path.isfile(lib32_dst):
                    manifest_32 = {
                        "file_format_version": "1.0.0",
                        "layer": {
                            "name": "VK_LAYER_SKYFRAME_framegen_32",
                            "type": "GLOBAL",
                            "library_path": "../../../lib/libVkLayer_skyframe_32.so",
                            "api_version": "1.3.0",
                            "implementation_version": "1",
                            "description": "SkyFrame Native AI Frame Generation Layer (32-bit)",
                            "functions": {
                                "vkGetInstanceProcAddr": "skyframe_GetInstanceProcAddr",
                                "vkGetDeviceProcAddr": "skyframe_GetDeviceProcAddr"
                            },
                            "disable_environment": {
                                "DISABLE_SKYFRAME": "1"
                            }
                        }
                    }
                    with open(VULKAN_LAYER_32_PATH, "w", encoding="utf-8") as f:
                        json.dump(manifest_32, f, indent=2)
                    logging.info(f"[SkyFrame] Registered 32-bit Vulkan layer at {VULKAN_LAYER_32_PATH}")
            else:
                # Remove manifests when disabled
                if os.path.isfile(VULKAN_LAYER_64_PATH):
                    os.remove(VULKAN_LAYER_64_PATH)
                if os.path.isfile(VULKAN_LAYER_32_PATH):
                    os.remove(VULKAN_LAYER_32_PATH)
                logging.info("[SkyFrame] Vulkan layer manifests removed (disabled).")
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
        return {
            "base_fps": 30.0 if not self.config["enabled"] else 40.0,
            "output_fps": 30.0 if not self.config["enabled"] else 80.0
        }
