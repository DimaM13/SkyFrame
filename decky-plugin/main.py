import os
import json
import logging
import shutil
import asyncio

logging.basicConfig(level=logging.INFO)

def get_target_user_home() -> str:
    """Finds the actual SteamOS user home directory (usually /home/deck)"""
    for candidate in [
        os.environ.get("DECKY_USER_HOME"),
        "/home/deck",
        os.path.expanduser("~")
    ]:
        if candidate and os.path.isdir(candidate):
            return candidate
    return "/home/deck"

USER_HOME = get_target_user_home()
CONFIG_DIR = os.path.join(USER_HOME, ".config", "skyframe")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")
LOCAL_LIB_DIR = os.path.join(USER_HOME, ".local", "lib")
USER_LAYER_DIR = os.path.join(USER_HOME, ".local", "share", "vulkan", "implicit_layer.d")
SYSTEM_LAYER_DIR = "/etc/vulkan/implicit_layer.d"
MODELS_DIR = os.path.join(USER_HOME, ".local", "share", "skyframe", "models")
LOG_DIR = os.path.join(USER_HOME, ".local", "share", "skyframe")

DEFAULT_CONFIG = {
    "enabled": False,
    "global_injection": False, # If true, inject into all games without requiring ENABLE_SKYFRAME=1
    "mode": 1,                 # 0 = Lite (180p), 1 = Balanced (240p), 2 = Quality (360p)
    "multiplier": 2,           # 2x frame generation
    "hud_protection": True,
    "hud_threshold": 0.08,
    "show_hud": False
}

def get_target_uid_gid():
    try:
        st = os.stat(USER_HOME)
        return st.st_uid, st.st_gid
    except Exception:
        return 1000, 1000

def fix_perms(path: str, is_exec: bool = False):
    """Ensures created files/directories belong to user deck (UID 1000) and are readable/executable"""
    try:
        uid, gid = get_target_uid_gid()
        if os.getuid() == 0:
            try:
                os.chown(path, uid, gid)
            except Exception:
                pass
        mode = 0o755 if (is_exec or os.path.isdir(path)) else 0o664
        os.chmod(path, mode)
    except Exception as e:
        logging.warning(f"[SkyFrame] Failed to set perms on {path}: {e}")

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
            fix_perms(CONFIG_DIR)
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(self.config, f, indent=2)
            fix_perms(CONFIG_PATH)
            self.sync_vulkan_layer()
        except Exception as e:
            logging.error(f"[SkyFrame] Failed to save config: {e}")

    def sync_vulkan_layer(self):
        """Installs layer libraries into ~/.local/lib and registers implicit layer manifests"""
        try:
            plugin_dir = os.path.dirname(os.path.abspath(__file__))
            bin_dir = os.path.join(plugin_dir, "bin")

            os.makedirs(LOCAL_LIB_DIR, exist_ok=True)
            fix_perms(LOCAL_LIB_DIR, is_exec=True)

            os.makedirs(USER_LAYER_DIR, exist_ok=True)
            fix_perms(USER_LAYER_DIR, is_exec=True)

            os.makedirs(MODELS_DIR, exist_ok=True)
            fix_perms(MODELS_DIR, is_exec=True)

            os.makedirs(LOG_DIR, exist_ok=True)
            fix_perms(LOG_DIR, is_exec=True)

            # Copy models to ~/.local/share/skyframe/models
            models_src = os.path.join(bin_dir, "models")
            if os.path.isdir(models_src):
                for mf in os.listdir(models_src):
                    src_f = os.path.join(models_src, mf)
                    dst_f = os.path.join(MODELS_DIR, mf)
                    if os.path.isfile(src_f) and not os.path.isfile(dst_f):
                        shutil.copy2(src_f, dst_f)
                        fix_perms(dst_f)

            # Copy 64-bit library
            lib64_src = os.path.join(bin_dir, "libVkLayer_skyframe.so")
            lib64_dst = os.path.join(LOCAL_LIB_DIR, "libVkLayer_skyframe.so")
            if os.path.isfile(lib64_src):
                shutil.copy2(lib64_src, lib64_dst)
                fix_perms(lib64_dst, is_exec=True)

            # Copy 32-bit library (for 32-bit games like Supreme Commander 2)
            lib32_src = os.path.join(bin_dir, "libVkLayer_skyframe_32.so")
            lib32_dst = os.path.join(LOCAL_LIB_DIR, "libVkLayer_skyframe_32.so")
            if os.path.isfile(lib32_src):
                shutil.copy2(lib32_src, lib32_dst)
                fix_perms(lib32_dst, is_exec=True)

            # Manifest destination paths
            user_manifest_64 = os.path.join(USER_LAYER_DIR, "vk_layer_skyframe.json")
            user_manifest_32 = os.path.join(USER_LAYER_DIR, "vk_layer_skyframe_32.json")
            sys_manifest_64 = os.path.join(SYSTEM_LAYER_DIR, "vk_layer_skyframe.json")
            sys_manifest_32 = os.path.join(SYSTEM_LAYER_DIR, "vk_layer_skyframe_32.json")

            if self.config.get("enabled", False):
                global_mode = self.config.get("global_injection", False)

                # 64-bit layer definition
                layer_64 = {
                    "name": "VK_LAYER_SKYFRAME_framegen",
                    "type": "GLOBAL",
                    "library_path": lib64_dst,
                    "api_version": "1.3.0",
                    "implementation_version": "1",
                    "description": "SkyFrame Native AI Frame Generation Layer (64-bit)",
                    "functions": {
                        "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion",
                        "vkGetInstanceProcAddr": "skyframe_GetInstanceProcAddr",
                        "vkGetDeviceProcAddr": "skyframe_GetDeviceProcAddr"
                    },
                    "disable_environment": {
                        "DISABLE_SKYFRAME": "1"
                    }
                }
                if not global_mode:
                    layer_64["enable_environment"] = { "ENABLE_SKYFRAME": "1" }

                manifest_64 = {
                    "file_format_version": "1.0.0",
                    "layer": layer_64
                }

                # 32-bit layer definition (uses same layer name for canonical multilib match)
                layer_32 = {
                    "name": "VK_LAYER_SKYFRAME_framegen",
                    "type": "GLOBAL",
                    "library_path": lib32_dst,
                    "api_version": "1.3.0",
                    "implementation_version": "1",
                    "description": "SkyFrame Native AI Frame Generation Layer (32-bit)",
                    "functions": {
                        "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion",
                        "vkGetInstanceProcAddr": "skyframe_GetInstanceProcAddr",
                        "vkGetDeviceProcAddr": "skyframe_GetDeviceProcAddr"
                    },
                    "disable_environment": {
                        "DISABLE_SKYFRAME": "1"
                    }
                }
                if not global_mode:
                    layer_32["enable_environment"] = { "ENABLE_SKYFRAME": "1" }

                manifest_32 = {
                    "file_format_version": "1.0.0",
                    "layer": layer_32
                }

                # 1. Write user manifests (~/.local/share/vulkan/implicit_layer.d/)
                with open(user_manifest_64, "w", encoding="utf-8") as f:
                    json.dump(manifest_64, f, indent=2)
                fix_perms(user_manifest_64)

                if os.path.isfile(lib32_dst):
                    with open(user_manifest_32, "w", encoding="utf-8") as f:
                        json.dump(manifest_32, f, indent=2)
                    fix_perms(user_manifest_32)

                # 2. Write system manifests (/etc/vulkan/implicit_layer.d/) if root
                try:
                    if os.getuid() == 0:
                        os.makedirs(SYSTEM_LAYER_DIR, exist_ok=True)
                        with open(sys_manifest_64, "w", encoding="utf-8") as f:
                            json.dump(manifest_64, f, indent=2)
                        os.chmod(sys_manifest_64, 0o644)

                        if os.path.isfile(lib32_dst):
                            with open(sys_manifest_32, "w", encoding="utf-8") as f:
                                json.dump(manifest_32, f, indent=2)
                            os.chmod(sys_manifest_32, 0o644)
                except Exception as e:
                    logging.warning(f"[SkyFrame] Could not write system manifest: {e}")

                logging.info(f"[SkyFrame] Layers registered successfully. Global injection: {global_mode}")
            else:
                # Remove manifests when disabled
                for p in [user_manifest_64, user_manifest_32, sys_manifest_64, sys_manifest_32]:
                    if os.path.isfile(p):
                        try:
                            os.remove(p)
                        except Exception:
                            pass
                logging.info("[SkyFrame] Vulkan layer manifests removed (disabled).")
        except Exception as e:
            logging.error(f"[SkyFrame] Error updating Vulkan layer: {e}")

    async def _main(self):
        logging.info(f"[SkyFrame] Backend daemon started. Target user home: {USER_HOME}")
        self.sync_vulkan_layer()

    async def _unload(self):
        logging.info("[SkyFrame] Backend daemon unloading...")

    async def get_config(self):
        return self.config

    async def set_enabled(self, enabled: bool):
        self.config["enabled"] = bool(enabled)
        self.save_config()
        return self.config

    async def set_global_injection(self, global_injection: bool):
        self.config["global_injection"] = bool(global_injection)
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

    async def set_show_hud(self, show_hud: bool):
        self.config["show_hud"] = bool(show_hud)
        self.save_config()
        return self.config

    async def get_stats(self):
        return {
            "base_fps": 30.0 if not self.config["enabled"] else 40.0,
            "output_fps": 30.0 if not self.config["enabled"] else 80.0
        }

    async def get_status_info(self):
        return {
            "user_home": USER_HOME,
            "lib64": os.path.isfile(os.path.join(LOCAL_LIB_DIR, "libVkLayer_skyframe.so")),
            "lib32": os.path.isfile(os.path.join(LOCAL_LIB_DIR, "libVkLayer_skyframe_32.so")),
            "user_manifest": os.path.isfile(os.path.join(USER_LAYER_DIR, "vk_layer_skyframe.json")),
            "sys_manifest": os.path.isfile(os.path.join(SYSTEM_LAYER_DIR, "vk_layer_skyframe.json")),
            "enabled": self.config.get("enabled", False),
            "global_mode": self.config.get("global_injection", False)
        }
