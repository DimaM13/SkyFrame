import {
  ButtonItem,
  PanelSection,
  PanelSectionRow,
  ToggleField,
  DropdownItem,
  staticClasses
} from "@decky/ui";
import {
  callable,
  definePlugin,
  toaster
} from "@decky/api";
import { useState, useEffect } from "react";
import { FaBolt, FaCopy, FaFileAlt, FaTerminal } from "react-icons/fa";

interface SkyFrameConfig {
  enabled: boolean;
  global_injection?: boolean;
  mode: number;
  hud_protection: boolean;
  multiplier: number;
  target_hz?: number;
  show_hud?: boolean;
}

interface StatusInfo {
  user_home: string;
  lib64: boolean;
  lib32: boolean;
  user_manifest: boolean;
  sys_manifest: boolean;
  enabled: boolean;
  global_mode: boolean;
}

const getConfig = callable<[], SkyFrameConfig>("get_config");
const setEnabledCall = callable<[enabled: boolean], SkyFrameConfig>("set_enabled");
const setGlobalInjectionCall = callable<[global_injection: boolean], SkyFrameConfig>("set_global_injection");
const setModeCall = callable<[mode: number], SkyFrameConfig>("set_mode");
const setHudProtectionCall = callable<[hud_protection: boolean], SkyFrameConfig>("set_hud_protection");
const setShowHudCall = callable<[show_hud: boolean], SkyFrameConfig>("set_show_hud");
const setTargetHzCall = callable<[target_hz: number], SkyFrameConfig>("set_target_hz");
const getStatsCall = callable<[], { base_fps: number; output_fps: number; target_hz?: number }>("get_stats");
const getStatusInfoCall = callable<[], StatusInfo>("get_status_info");

function Content() {
  const [enabled, setEnabled] = useState<boolean>(false);
  const [globalMode, setGlobalMode] = useState<boolean>(false);
  const [mode, setMode] = useState<number>(1);
  const [hudProtection, setHudProtection] = useState<boolean>(true);
  const [showHud, setShowHud] = useState<boolean>(false);
  const [targetHz, setTargetHz] = useState<number>(60);
  const [baseFps, setBaseFps] = useState<number>(30);
  const [outputFps, setOutputFps] = useState<number>(60);
  const [status, setStatus] = useState<StatusInfo | null>(null);

  useEffect(() => {
    getConfig().then((res) => {
      if (res) {
        setEnabled(Boolean(res.enabled));
        setGlobalMode(Boolean(res.global_injection ?? false));
        setMode(Number(res.mode ?? 1));
        setHudProtection(Boolean(res.hud_protection ?? true));
        setShowHud(Boolean(res.show_hud ?? false));
        if (res.target_hz) setTargetHz(Number(res.target_hz));
      }
    }).catch((e) => console.error("[SkyFrame] Error loading config:", e));

    const checkRefreshRate = () => {
      try {
        const steamClient = (window as any).SteamClient;
        if (steamClient?.Settings?.gamescope_display_refresh_rate) {
          const hz = steamClient.Settings.gamescope_display_refresh_rate();
          if (hz && hz >= 30 && hz <= 240) {
            setTargetHz(Math.round(hz));
            setTargetHzCall(Math.round(hz)).catch(() => {});
            return;
          }
        }
        if (steamClient?.System?.Perf?.GetSettings) {
          steamClient.System.Perf.GetSettings().then((perf: any) => {
            const hz = perf?.display_external_refresh_manual_hz || perf?.display_refresh_manual_hz;
            if (hz && hz >= 30 && hz <= 240) {
              setTargetHz(Math.round(hz));
              setTargetHzCall(Math.round(hz)).catch(() => {});
            }
          }).catch(() => {});
        }
      } catch (e) {
        console.warn("[SkyFrame] Could not query Steam refresh rate:", e);
      }
    };
    checkRefreshRate();

    const loadStatus = () => {
      getStatusInfoCall().then(setStatus).catch(() => {});
    };
    loadStatus();

    const interval = setInterval(() => {
      getStatsCall().then((stats) => {
        if (stats) {
          setBaseFps(Math.round(stats.base_fps));
          setOutputFps(Math.round(stats.output_fps));
          if (stats.target_hz) setTargetHz(Math.round(stats.target_hz));
        }
      }).catch(() => {});
      loadStatus();
      checkRefreshRate();
    }, 2000);

    return () => clearInterval(interval);
  }, []);

  const handleToggle = (val: boolean) => {
    setEnabled(val);
    setEnabledCall(val).catch(console.error);
    setTimeout(() => getStatusInfoCall().then(setStatus).catch(() => {}), 500);
  };

  const handleGlobalToggle = (val: boolean) => {
    setGlobalMode(val);
    setGlobalInjectionCall(val).catch(console.error);
    setTimeout(() => getStatusInfoCall().then(setStatus).catch(() => {}), 500);
  };

  const handleModeChange = (val: number) => {
    setMode(val);
    setModeCall(val).catch(console.error);
  };

  const handleHudChange = (val: boolean) => {
    setHudProtection(val);
    setHudProtectionCall(val).catch(console.error);
  };

  const handleShowHudChange = (val: boolean) => {
    setShowHud(val);
    setShowHudCall(val).catch(console.error);
  };

  const copyToClipboard = (cmd: string, desc: string) => {
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(cmd);
      } else {
        const textArea = document.createElement("textarea");
        textArea.value = cmd;
        document.body.appendChild(textArea);
        textArea.select();
        document.execCommand("copy");
        document.body.removeChild(textArea);
      }
      toaster.toast({
        title: "SkyFrame",
        body: `Скопировано: ${desc}`
      });
    } catch (err) {
      console.error("[SkyFrame] Failed to copy:", err);
    }
  };

  const modeOptions = [
    { data: 0, label: "🚀 Лёгкий (180p) — для тяжелых AAA" },
    { data: 1, label: "⚖️ Баланс (240p) — рекомендуемый" },
    { data: 2, label: "💎 Качество (360p) — макс. резкость" }
  ];

  return (
    <>
      <PanelSection title="SkyFrame: Нативная генерация">
        <PanelSectionRow>
          <ToggleField
            label="Генерация кадров (2x)"
            description="Аппаратный сдвиг пикселей Vulkan FastWarp"
            checked={enabled}
            onChange={handleToggle}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <ToggleField
            label="Глобальный режим (без параметров)"
            description="Слой активен во ВСЕХ играх без указания параметров в Steam"
            checked={globalMode}
            onChange={handleGlobalToggle}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <DropdownItem
            label="Профиль вычислений"
            rgOptions={modeOptions}
            selectedOption={mode}
            onChange={(opt) => handleModeChange(Number(opt.data))}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <ToggleField
            label="Защита интерфейса (HUD)"
            description="Предотвращает артефакты на прицелах и миникарте"
            checked={hudProtection}
            onChange={handleHudChange}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <ToggleField
            label="Визуальный маркер кадров"
            description="Зелёная точка в углу для проверки вывода промежуточных кадров"
            checked={showHud}
            onChange={handleShowHudChange}
          />
        </PanelSectionRow>

        {enabled && (
          <PanelSectionRow>
            <div style={{
              background: "rgba(255, 255, 255, 0.05)",
              borderRadius: "8px",
              padding: "10px",
              fontSize: "12px",
              display: "flex",
              flexDirection: "column",
              gap: "6px"
            }}>
              <div style={{ display: "flex", justifyContent: "space-between" }}>
                <span style={{ color: "#aaa" }}>Статус:</span>
                <span style={{ color: "#4ade80", fontWeight: "bold" }}>● Активно (Vulkan FastWarp)</span>
              </div>
              <div style={{ display: "flex", justifyContent: "space-between" }}>
                <span style={{ color: "#aaa" }}>Частота кадров:</span>
                <span style={{ color: "#f59e0b", fontWeight: "bold" }}>
                  {baseFps} FPS → {outputFps} FPS ({targetHz} Hz Auto)
                </span>
              </div>
              <div style={{ display: "flex", justifyContent: "space-between" }}>
                <span style={{ color: "#aaa" }}>Движок:</span>
                <span style={{ color: "#fff" }}>RIFE 4.6 + RDNA2 FP16</span>
              </div>
              {status && (
                <div style={{ display: "flex", justifyContent: "space-between", borderTop: "1px solid rgba(255,255,255,0.1)", paddingTop: "4px" }}>
                  <span style={{ color: "#aaa" }}>Поддержка игр:</span>
                  <span style={{ color: status.lib32 ? "#4ade80" : "#f87171" }}>
                    64-bit: ✓ | 32-bit: {status.lib32 ? "✓" : "✗"}
                  </span>
                </div>
              )}
            </div>
          </PanelSectionRow>
        )}
      </PanelSection>

      <PanelSection title="Параметры запуска Steam">
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            onClick={() => copyToClipboard("ENABLE_SKYFRAME=1 %command%", "Команда запуска")}
          >
            📋 Скопировать ENABLE_SKYFRAME=1
          </ButtonItem>
        </PanelSectionRow>

        <PanelSectionRow>
          <ButtonItem
            layout="below"
            onClick={() => copyToClipboard("VK_LOADER_DEBUG=all ENABLE_SKYFRAME=1 %command% > ~/skyframe_game.log 2>&1", "Диагностический запуск")}
          >
            🛠️ Скопировать запуск с подробным логом Vulkan
          </ButtonItem>
        </PanelSectionRow>

        <PanelSectionRow>
          <ButtonItem
            layout="below"
            onClick={() => copyToClipboard("cat /tmp/skyframe.log", "Команда чтения логов")}
          >
            📄 Скопировать команду чтения логов (/tmp/skyframe.log)
          </ButtonItem>
        </PanelSectionRow>
      </PanelSection>
    </>
  );
}

export default definePlugin(() => {
  return {
    name: "SkyFrame",
    titleView: <div className={staticClasses.Title}>SkyFrame</div>,
    content: <Content />,
    icon: <FaBolt />,
    onDismount() {}
  };
});
