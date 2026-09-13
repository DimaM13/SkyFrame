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
LOCAL_BIN_DIR = os.path.join(USER_HOME, ".local", "bin")
USER_LAYER_DIR = os.path.join(USER_HOME, ".local", "share", "vulkan", "implicit_layer.d")
SYSTEM_LAYER_DIR = "/etc/vulkan/implicit_layer.d"
LOG_DIR = os.path.join(USER_HOME, ".local", "share", "skyframe")
LSFG_CONFIG_DIR = os.path.join(USER_HOME, ".config", "lsfg-vk")
LSFG_CONFIG_PATH = os.path.join(LSFG_CONFIG_DIR, "conf.toml")

DEFAULT_CONFIG = {
    "enabled": False,
    "global_injection": False, # If true, inject into all games without requiring launch options
    "multiplier": 2,           # 2x, 3x, 4x frame generation
    "performance_mode": True,  # LSFG FP16 Rapid Packed Math on AMD RDNA2 GPU (~4ms latency)
    "target_hz": 60,           # Target display refresh rate (synced with Deck QAM)
    "flow_scale": 0.90,        # Flow scale (0.50 to 1.00, default 0.90)
    "custom_dll_path": ""      # Custom path to Lossless.dll if not in standard Steam library
}

def detect_system_refresh_rate() -> int:
    """Attempts to detect the active display refresh rate via xrandr or gamescope"""
    try:
        import subprocess
        for disp in [":0", ":1"]:
            env = os.environ.copy()
            env["DISPLAY"] = disp
            res = subprocess.run(["xrandr"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=1, env=env)
            if res.returncode == 0 and res.stdout:
                for line in res.stdout.splitlines():
                    if "*" in line:
                        parts = line.split()
                        for p in parts:
                            if "*" in p:
                                hz_str = p.replace("*", "").replace("+", "")
                                hz = float(hz_str)
                                if 30 <= hz <= 240:
                                    return int(round(hz))
    except Exception:
        pass
    return 60

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

def find_lossless_dll(custom_path: str = "") -> dict:
    """Locates Lossless.dll from Steam installations, MicroSD cards, or custom path"""
    candidates = []
    if custom_path and os.path.isfile(custom_path):
        candidates.append(custom_path)

    steam_rel = os.path.join("steamapps", "common", "Lossless Scaling", "Lossless.dll")

    # Standard locations on Linux / SteamOS
    candidates.extend([
        os.path.join(USER_HOME, ".local", "share", "Steam", steam_rel),
        os.path.join(USER_HOME, ".steam", "steam", steam_rel),
        os.path.join(USER_HOME, ".steam", "root", steam_rel),
        os.path.join(USER_HOME, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam", steam_rel),
    ])

    # MicroSD / Removable storage search
    for media_root in ["/run/media/mmcblk0p1", f"/run/media/{os.path.basename(USER_HOME)}", "/run/media/deck"]:
        if os.path.isdir(media_root):
            candidates.append(os.path.join(media_root, steam_rel))
            try:
                for sub in os.listdir(media_root):
                    sub_path = os.path.join(media_root, sub)
                    if os.path.isdir(sub_path):
                        candidates.append(os.path.join(sub_path, steam_rel))
            except Exception:
                pass

    for path in candidates:
        if os.path.isfile(path):
            try:
                size_mb = round(os.path.getsize(path) / (1024 * 1024), 2)
                return {
                    "found": True,
                    "path": path,
                    "size_mb": size_mb,
                    "status": "Found (Ready for LSFG 2.x FP16)",
                    "branch_hint": "Please ensure you selected the 'lsfg-vk' beta branch in Steam for Lossless Scaling."
                }
            except Exception:
                pass

    return {
        "found": False,
        "path": "",
        "size_mb": 0,
        "status": "Not found in Steam library",
        "branch_hint": "Install Lossless Scaling on Steam and select the 'lsfg-vk' beta branch in Properties -> Betas."
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
            fix_perms(CONFIG_DIR)
            tmp_path = CONFIG_PATH + ".tmp"
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(self.config, f, indent=2)
            fix_perms(tmp_path)
            os.replace(tmp_path, CONFIG_PATH)
            fix_perms(CONFIG_PATH)
        except Exception as e:
            logging.error(f"[SkyFrame] Failed to save config: {e}")

    def sync_lsfg_config(self, dll_path: str):
        """Generates ~/.config/lsfg-vk/conf.toml for LSFG engine"""
        try:
            os.makedirs(LSFG_CONFIG_DIR, exist_ok=True)
            fix_perms(LSFG_CONFIG_DIR)

            multiplier = int(self.config.get("multiplier", 2))
            flow_scale = float(self.config.get("flow_scale", 0.90))
            perf_mode = "true" if self.config.get("performance_mode", True) else "false"

            toml_lines = [
                "# Automatically managed by SkyFrame Decky Plugin",
                "[global]",
                f'dll = "{dll_path}"',
                "allow_fp16 = true",
                "",
                "[[profile]]",
                'name = "Default"',
                'active_in = ["*"]',
                f"multiplier = {multiplier}",
                f"flow_scale = {flow_scale}",
                f"performance_mode = {perf_mode}",
                'pacing = "none"',
                ""
            ]
            toml_content = "\n".join(toml_lines)

            tmp_toml = LSFG_CONFIG_PATH + ".tmp"
            with open(tmp_toml, "w", encoding="utf-8") as f:
                f.write(toml_content)
            fix_perms(tmp_toml)
            os.replace(tmp_toml, LSFG_CONFIG_PATH)
            fix_perms(LSFG_CONFIG_PATH)
            logging.info(f"[SkyFrame] Updated LSFG conf.toml (multiplier: {multiplier}, perf: {perf_mode})")
        except Exception as e:
            logging.warning(f"[SkyFrame] Failed to sync LSFG conf.toml: {e}")

    def sync_vulkan_layer(self):
        """Installs layer libraries into ~/.local/lib and registers implicit layer manifests"""
        try:
            plugin_dir = os.path.dirname(os.path.abspath(__file__))
            bin_dir = os.path.join(plugin_dir, "bin")

            os.makedirs(LOCAL_LIB_DIR, exist_ok=True)
            fix_perms(LOCAL_LIB_DIR, is_exec=True)

            os.makedirs(LOCAL_BIN_DIR, exist_ok=True)
            fix_perms(LOCAL_BIN_DIR, is_exec=True)

            os.makedirs(USER_LAYER_DIR, exist_ok=True)
            fix_perms(USER_LAYER_DIR, is_exec=True)

            os.makedirs(LOG_DIR, exist_ok=True)
            fix_perms(LOG_DIR, is_exec=True)

            # Clean up any legacy experimental skyframe manifests to avoid collisions
            for old_mf in ["vk_layer_skyframe.json", "vk_layer_skyframe_32.json"]:
                p = os.path.join(USER_LAYER_DIR, old_mf)
                if os.path.isfile(p):
                    try: os.remove(p)
                    except Exception: pass

            # Copy LSFG engine binaries (64-bit and 32-bit)
            for lib_name in ["liblsfg-vk-layer.so", "liblsfg-vk-layer_32.so"]:
                src = os.path.join(bin_dir, lib_name)
                dst = os.path.join(LOCAL_LIB_DIR, lib_name)
                if os.path.isfile(src):
                    shutil.copy2(src, dst)
                    fix_perms(dst, is_exec=True)

            # Copy lsfg-vk-cli diagnostic tool
            cli_src = os.path.join(bin_dir, "lsfg-vk-cli")
            cli_dst = os.path.join(LOCAL_BIN_DIR, "lsfg-vk-cli")
            if os.path.isfile(cli_src):
                shutil.copy2(cli_src, cli_dst)
                fix_perms(cli_dst, is_exec=True)

            # Check Lossless.dll location and sync conf.toml
            custom_dll = self.config.get("custom_dll_path", "")
            ls_info = find_lossless_dll(custom_dll)
            if ls_info["found"]:
                self.sync_lsfg_config(ls_info["path"])

            is_enabled = self.config.get("enabled", False)
            global_mode = self.config.get("global_injection", False)

            lsfg_lib64 = os.path.join(LOCAL_LIB_DIR, "liblsfg-vk-layer.so")
            lsfg_lib32 = os.path.join(LOCAL_LIB_DIR, "liblsfg-vk-layer_32.so")

            # --- LSFG Layer Manifest ---
            lsfg_layer_64 = {
                "name": "VK_LAYER_LSFGVK_frame_generation",
                "type": "GLOBAL",
                "library_path": lsfg_lib64,
                "api_version": "1.3.0",
                "implementation_version": "2",
                "description": "Lossless Scaling Frame Generation Layer (64-bit)",
                "disable_environment": { "DISABLE_LSFGVK": "0" if not is_enabled else "1" }
            }
            if is_enabled and not global_mode:
                lsfg_layer_64["enable_environment"] = { "ENABLE_LSFGVK": "1" }

            lsfg_layer_32 = dict(lsfg_layer_64)
            lsfg_layer_32["library_path"] = lsfg_lib32
            lsfg_layer_32["description"] = "Lossless Scaling Frame Generation Layer (32-bit)"

            # Write user layer manifests
            manifest_files = [
                (os.path.join(USER_LAYER_DIR, "VkLayer_LSFGVK_frame_generation.json"), lsfg_layer_64),
                (os.path.join(USER_LAYER_DIR, "VkLayer_LSFGVK_frame_generation_32.json"), lsfg_layer_32),
            ]

            for mf_path, l_def in manifest_files:
                with open(mf_path, "w", encoding="utf-8") as f:
                    json.dump({"file_format_version": "1.0.0", "layer": l_def}, f, indent=2)
                fix_perms(mf_path)

            # If running as root, also mirror to /etc/vulkan/implicit_layer.d/
            if os.getuid() == 0:
                try:
                    os.makedirs(SYSTEM_LAYER_DIR, exist_ok=True)
                    for mf_path, l_def in manifest_files:
                        sys_path = os.path.join(SYSTEM_LAYER_DIR, os.path.basename(mf_path))
                        with open(sys_path, "w", encoding="utf-8") as f:
                            json.dump({"file_format_version": "1.0.0", "layer": l_def}, f, indent=2)
                        os.chmod(sys_path, 0o644)
                except Exception as e:
                    logging.warning(f"[SkyFrame] Could not write system manifest: {e}")

            logging.info(f"[SkyFrame] LSFG layer sync complete. Enabled: {is_enabled}, Global: {global_mode}")
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
        self.sync_vulkan_layer()
        return self.config

    async def set_multiplier(self, multiplier: int):
        if multiplier in [2, 3, 4]:
            self.config["multiplier"] = int(multiplier)
            self.save_config()
            self.sync_vulkan_layer()
        return self.config

    async def set_performance_mode(self, performance_mode: bool):
        self.config["performance_mode"] = bool(performance_mode)
        self.save_config()
        self.sync_vulkan_layer()
        return self.config

    async def set_global_injection(self, global_injection: bool):
        self.config["global_injection"] = bool(global_injection)
        self.save_config()
        self.sync_vulkan_layer()
        return self.config

    async def set_target_hz(self, hz: int):
        if hz >= 30 and hz <= 240:
            self.config["target_hz"] = int(hz)
            self.save_config()
        return self.config

    async def set_flow_scale(self, flow_scale: float):
        try:
            val = float(flow_scale)
            if 0.50 <= val <= 1.00:
                self.config["flow_scale"] = round(val, 2)
                self.save_config()
                self.sync_vulkan_layer()
        except Exception as e:
            logging.error(f"[SkyFrame] Failed to set flow_scale: {e}")
        return self.config

    async def set_custom_dll_path(self, path: str):
        self.config["custom_dll_path"] = str(path).strip()
        self.save_config()
        self.sync_vulkan_layer()
        return self.config

    async def get_lossless_status(self):
        custom_dll = self.config.get("custom_dll_path", "")
        return find_lossless_dll(custom_dll)

    async def get_stats(self):
        try:
            if os.path.isfile("/tmp/skyframe_stats.json"):
                with open("/tmp/skyframe_stats.json", "r", encoding="utf-8") as f:
                    data = json.load(f)
                    if isinstance(data, dict):
                        return {
                            "base_fps": float(data.get("base_fps", 0.0)),
                            "output_fps": float(data.get("output_fps", 0.0)),
                            "target_hz": int(data.get("target_hz", self.config.get("target_hz", 60)))
                        }
        except Exception:
            pass
        mult = self.config.get("multiplier", 2)
        return {
            "base_fps": 0.0 if not self.config.get("enabled", False) else 30.0,
            "output_fps": 0.0 if not self.config.get("enabled", False) else (30.0 * mult),
            "target_hz": int(self.config.get("target_hz", 60))
        }

    async def get_status_info(self):
        return {
            "user_home": USER_HOME,
            "lib64": os.path.isfile(os.path.join(LOCAL_LIB_DIR, "liblsfg-vk-layer.so")),
            "lib32": os.path.isfile(os.path.join(LOCAL_LIB_DIR, "liblsfg-vk-layer_32.so")),
            "enabled": self.config.get("enabled", False),
            "global_mode": self.config.get("global_injection", False),
            "ls_info": find_lossless_dll(self.config.get("custom_dll_path", ""))
        }

    async def copy_to_clipboard(self, text: str) -> bool:
        logging.info(f"[SkyFrame] Copying to system clipboard: {text}")
        uid, _ = get_target_uid_gid()
        for wdisp in ["wayland-0", "wayland-1"]:
            for rdir in [f"/run/user/{uid}", "/run/user/1000", "/tmp"]:
                env = os.environ.copy()
                env["XDG_RUNTIME_DIR"] = rdir
                env["WAYLAND_DISPLAY"] = wdisp
                try:
                    import subprocess
                    res = subprocess.run(["wl-copy"], input=text.encode("utf-8"), env=env, timeout=1)
                    if res.returncode == 0:
                        return True
                except Exception:
                    pass
        for xdisp in [":0", ":1"]:
            env = os.environ.copy()
            env["DISPLAY"] = xdisp
            try:
                import subprocess
                res = subprocess.run(["xclip", "-selection", "clipboard"], input=text.encode("utf-8"), env=env, timeout=1)
                if res.returncode == 0:
                    return True
            except Exception:
                pass
        return False
