import {
  definePlugin,
  PanelSection,
  PanelSectionRow,
  ToggleField,
  DropdownItem,
  ServerAPI,
  staticClasses
} from "decky-frontend-lib";
import { VFC, useState, useEffect } from "react";
import { FaBolt, FaRocket, FaShieldAlt } from "react-icons/fa";

interface SkyFrameConfig {
  enabled: boolean;
  mode: number; // 0 = Lite (180p), 1 = Balanced (240p), 2 = Quality (360p)
  hud_protection: boolean;
  multiplier: number;
}

const Content: VFC<{ serverAPI: ServerAPI }> = ({ serverAPI }) => {
  const [enabled, setEnabled] = useState<boolean>(false);
  const [mode, setMode] = useState<number>(1);
  const [hudProtection, setHudProtection] = useState<boolean>(true);
  const [baseFps, setBaseFps] = useState<number>(30);
  const [outputFps, setOutputFps] = useState<number>(60);

  // Load config on mount
  useEffect(() => {
    serverAPI.callPluginMethod<{}, SkyFrameConfig>("get_config", {}).then((res) => {
      if (res.success) {
        setEnabled(res.result.enabled);
        setMode(res.result.mode);
        setHudProtection(res.result.hud_protection);
      }
    });

    // Poll live telemetry every 1.5s
    const interval = setInterval(() => {
      serverAPI.callPluginMethod<{}, { base_fps: number; output_fps: number }>("get_stats", {}).then((res) => {
        if (res.success) {
          setBaseFps(Math.round(res.result.base_fps));
          setOutputFps(Math.round(res.result.output_fps));
        }
      });
    }, 1500);

    return () => clearInterval(interval);
  }, []);

  const handleToggle = (val: boolean) => {
    setEnabled(val);
    serverAPI.callPluginMethod("set_enabled", { enabled: val });
  };

  const handleModeChange = (val: number) => {
    setMode(val);
    serverAPI.callPluginMethod("set_mode", { mode: val });
  };

  const handleHudChange = (val: boolean) => {
    setHudProtection(val);
    serverAPI.callPluginMethod("set_hud_protection", { hud_protection: val });
  };

  const modeOptions = [
    { data: 0, label: "🚀 Лёгкий (180p) — для AAA-игр" },
    { data: 1, label: "⚖️ Баланс (240p) — рекомендуемый" },
    { data: 2, label: "💎 Качество (360p) — макс. резкость" }
  ];

  return (
    <PanelSection title="SkyFrame: Нативная генерация кадров">
      {/* Master Toggle */}
      <PanelSectionRow>
        <ToggleField
          label="Генерация кадров (2x)"
          description="Аппаратный сдвиг пикселей Vulkan FastWarp"
          checked={enabled}
          onChange={handleToggle}
        />
      </PanelSectionRow>

      {/* Preset Mode Selection */}
      <PanelSectionRow>
        <DropdownItem
          label="Профиль вычислений"
          rgOptions={modeOptions}
          selectedOption={mode}
          onChange={(opt) => handleModeChange(opt.data)}
        />
      </PanelSectionRow>

      {/* HUD & Crosshair Protection */}
      <PanelSectionRow>
        <ToggleField
          label="Защита интерфейса (HUD)"
          description="Предотвращает двоение прицелов и текста миникарты"
          checked={hudProtection}
          onChange={handleHudChange}
        />
      </PanelSectionRow>

      {/* Real-time Telemetry Card */}
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
                {baseFps} FPS → {outputFps} FPS
              </span>
            </div>
            <div style={{ display: "flex", justifyContent: "space-between" }}>
              <span style={{ color: "#aaa" }}>Движок интерполяции:</span>
              <span style={{ color: "#fff" }}>RIFE 4.6 + RDNA2 FP16</span>
            </div>
          </div>
        </PanelSectionRow>
      )}
    </PanelSection>
  );
};

export default definePlugin((serverAPI: ServerAPI) => {
  return {
    title: <div className={staticClasses.Title}>SkyFrame</div>,
    content: <Content serverAPI={serverAPI} />,
    icon: <FaBolt />,
    onDismount() {}
  };
});
