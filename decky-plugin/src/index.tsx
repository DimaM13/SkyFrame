import {
  PanelSection,
  PanelSectionRow,
  ToggleField,
  DropdownItem,
  staticClasses
} from "@decky/ui";
import {
  callable,
  definePlugin
} from "@decky/api";
import { useState, useEffect } from "react";
import { FaBolt } from "react-icons/fa";

interface SkyFrameConfig {
  enabled: boolean;
  mode: number;
  hud_protection: boolean;
  multiplier: number;
}

const getConfig = callable<[], SkyFrameConfig>("get_config");
const setEnabledCall = callable<[enabled: boolean], SkyFrameConfig>("set_enabled");
const setModeCall = callable<[mode: number], SkyFrameConfig>("set_mode");
const setHudProtectionCall = callable<[hud_protection: boolean], SkyFrameConfig>("set_hud_protection");
const getStatsCall = callable<[], { base_fps: number; output_fps: number }>("get_stats");

function Content() {
  const [enabled, setEnabled] = useState<boolean>(false);
  const [mode, setMode] = useState<number>(1);
  const [hudProtection, setHudProtection] = useState<boolean>(true);
  const [baseFps, setBaseFps] = useState<number>(30);
  const [outputFps, setOutputFps] = useState<number>(60);

  useEffect(() => {
    getConfig().then((res) => {
      if (res) {
        setEnabled(Boolean(res.enabled));
        setMode(Number(res.mode ?? 1));
        setHudProtection(Boolean(res.hud_protection ?? true));
      }
    }).catch((e) => console.error("[SkyFrame] Error loading config:", e));

    const interval = setInterval(() => {
      getStatsCall().then((stats) => {
        if (stats) {
          setBaseFps(Math.round(stats.base_fps));
          setOutputFps(Math.round(stats.output_fps));
        }
      }).catch(() => {});
    }, 1500);

    return () => clearInterval(interval);
  }, []);

  const handleToggle = (val: boolean) => {
    setEnabled(val);
    setEnabledCall(val).catch(console.error);
  };

  const handleModeChange = (val: number) => {
    setMode(val);
    setModeCall(val).catch(console.error);
  };

  const handleHudChange = (val: boolean) => {
    setHudProtection(val);
    setHudProtectionCall(val).catch(console.error);
  };

  const modeOptions = [
    { data: 0, label: "🚀 Лёгкий (180p) — для AAA-игр" },
    { data: 1, label: "⚖️ Баланс (240p) — рекомендуемый" },
    { data: 2, label: "💎 Качество (360p) — макс. резкость" }
  ];

  return (
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
          description="Предотвращает двоение прицелов и текста миникарты"
          checked={hudProtection}
          onChange={handleHudChange}
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
