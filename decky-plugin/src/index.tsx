import {
  ButtonItem,
  PanelSection,
  PanelSectionRow,
  ToggleField,
  DropdownItem,
  SliderField,
  staticClasses
} from "@decky/ui";
import {
  callable,
  definePlugin,
  toaster
} from "@decky/api";
import { useState, useEffect } from "react";
import { FaBolt, FaCopy, FaFileAlt, FaTerminal, FaCheckCircle, FaExclamationTriangle } from "react-icons/fa";

interface SkyFrameConfig {
  enabled: boolean;
  engine?: string;
  global_injection?: boolean;
  mode: number;
  multiplier?: number;
  performance_mode?: boolean;
  hud_protection: boolean;
  target_hz?: number;
  show_hud?: boolean;
  flow_scale?: number;
  custom_dll_path?: string;
}

interface LosslessStatus {
  found: boolean;
  path: string;
  size_mb: number;
  status: string;
  branch_hint: string;
}

interface StatusInfo {
  user_home: string;
  lib64_sky: boolean;
  lib32_sky: boolean;
  lib64_lsfg: boolean;
  lib32_lsfg: boolean;
  enabled: boolean;
  engine: string;
  global_mode: boolean;
  ls_info: LosslessStatus;
}

const getConfig = callable<[], SkyFrameConfig>("get_config");
const setEnabledCall = callable<[enabled: boolean], SkyFrameConfig>("set_enabled");
const setEngineCall = callable<[engine: string], SkyFrameConfig>("set_engine");
const setMultiplierCall = callable<[multiplier: number], SkyFrameConfig>("set_multiplier");
const setPerformanceModeCall = callable<[performance_mode: boolean], SkyFrameConfig>("set_performance_mode");
const setGlobalInjectionCall = callable<[global_injection: boolean], SkyFrameConfig>("set_global_injection");
const setModeCall = callable<[mode: number], SkyFrameConfig>("set_mode");
const setHudProtectionCall = callable<[hud_protection: boolean], SkyFrameConfig>("set_hud_protection");
const setShowHudCall = callable<[show_hud: boolean], SkyFrameConfig>("set_show_hud");
const setTargetHzCall = callable<[target_hz: number], SkyFrameConfig>("set_target_hz");
const setFlowScaleCall = callable<[flow_scale: number], SkyFrameConfig>("set_flow_scale");
const copyToClipboardCall = callable<[text: string], boolean>("copy_to_clipboard");
const getStatsCall = callable<[], { base_fps: number; output_fps: number; target_hz?: number }>("get_stats");
const getStatusInfoCall = callable<[], StatusInfo>("get_status_info");
const getLosslessStatusCall = callable<[], LosslessStatus>("get_lossless_status");

function Content() {
  const [enabled, setEnabled] = useState<boolean>(false);
  const [engine, setEngine] = useState<string>("lsfg");
  const [globalMode, setGlobalMode] = useState<boolean>(false);
  const [multiplier, setMultiplier] = useState<number>(2);
  const [performanceMode, setPerformanceMode] = useState<boolean>(true);
  const [mode, setMode] = useState<number>(1);
  const [hudProtection, setHudProtection] = useState<boolean>(true);
  const [showHud, setShowHud] = useState<boolean>(false);
  const [targetHz, setTargetHz] = useState<number>(60);
  const [flowScale, setFlowScale] = useState<number>(90);
  const [baseFps, setBaseFps] = useState<number>(30);
  const [outputFps, setOutputFps] = useState<number>(60);
  const [status, setStatus] = useState<StatusInfo | null>(null);
  const [lsStatus, setLsStatus] = useState<LosslessStatus | null>(null);

  useEffect(() => {
    getConfig().then((res) => {
      if (res) {
        setEnabled(Boolean(res.enabled));
        setEngine(res.engine || "lsfg");
        setGlobalMode(Boolean(res.global_injection ?? false));
        setMultiplier(Number(res.multiplier ?? 2));
        setPerformanceMode(Boolean(res.performance_mode ?? true));
        setMode(Number(res.mode ?? 1));
        setHudProtection(Boolean(res.hud_protection ?? true));
        setShowHud(Boolean(res.show_hud ?? false));
        if (res.target_hz) setTargetHz(Number(res.target_hz));
        if (res.flow_scale !== undefined) setFlowScale(Math.round(Number(res.flow_scale) * 100));
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
      getStatusInfoCall().then((s) => {
        setStatus(s);
        if (s?.ls_info) setLsStatus(s.ls_info);
      }).catch(() => {});
      getLosslessStatusCall().then(setLsStatus).catch(() => {});
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
    }, 2500);

    return () => clearInterval(interval);
  }, []);

  const handleToggle = (val: boolean) => {
    setEnabled(val);
    setEnabledCall(val).catch(console.error);
    setTimeout(() => getStatusInfoCall().then(setStatus).catch(() => {}), 500);
  };

  const handleEngineChange = (item: any) => {
    const val = item.data;
    setEngine(val);
    setEngineCall(val).catch(console.error);
    setTimeout(() => getStatusInfoCall().then(setStatus).catch(() => {}), 500);
  };

  const handleMultiplierChange = (item: any) => {
    const val = Number(item.data);
    setMultiplier(val);
    setMultiplierCall(val).catch(console.error);
  };

  const handlePerformanceModeChange = (val: boolean) => {
    setPerformanceMode(val);
    setPerformanceModeCall(val).catch(console.error);
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

  const handleFlowScaleChange = (val: number) => {
    setFlowScale(val);
    setFlowScaleCall(val / 100.0).catch(console.error);
  };

  const copyToClipboard = (cmd: string, desc: string) => {
    try {
      if ((window as any).SteamClient?.System?.SetClipboardText) {
        (window as any).SteamClient.System.SetClipboardText(cmd);
      }
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(cmd).catch(() => {});
      }
      const textArea = document.createElement("textarea");
      textArea.value = cmd;
      document.body.appendChild(textArea);
      textArea.select();
      document.execCommand("copy");
      document.body.removeChild(textArea);

      copyToClipboardCall(cmd).catch(console.error);

      toaster.toast({
        title: "SkyFrame",
        body: `Скопировано: ${desc}`
      });
    } catch (err) {
      console.error("[SkyFrame] Failed to copy:", err);
    }
  };

  return (
    <>
      <PanelSection title="SkyFrame: Нативная генерация">
        <PanelSectionRow>
          <ToggleField
            label="Генерация кадров"
            description={engine === "lsfg" ? "Нейросетевая интерполяция LSFG (Lossless Scaling)" : "Аппаратный оптический поток DIS-Flow (Vulkan Compute)"}
            checked={enabled}
            onChange={handleToggle}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <DropdownItem
            label="Движок генерации"
            description="Выберите нейросетевую модель LSFG или встроенный DIS"
            menuLabel="Движок"
            rgOptions={[
              { label: "⭐ LSFG Нейросеть (Lossless Scaling)", data: "lsfg" },
              { label: "SkyFrame DIS (Нативный оптический поток)", data: "skyframe" }
            ]}
            selectedOption={engine}
            onChange={handleEngineChange}
          />
        </PanelSectionRow>

        {engine === "lsfg" && (
          <>
            <PanelSectionRow>
              <div style={{
                background: lsStatus?.found ? "rgba(34, 197, 94, 0.12)" : "rgba(239, 68, 68, 0.12)",
                border: `1px solid ${lsStatus?.found ? "#22c55e" : "#ef4444"}`,
                borderRadius: "8px",
                padding: "10px",
                fontSize: "12px",
                display: "flex",
                flexDirection: "column",
                gap: "6px"
              }}>
                <div style={{ display: "flex", alignItems: "center", gap: "6px", fontWeight: "bold", color: lsStatus?.found ? "#4ade80" : "#f87171" }}>
                  {lsStatus?.found ? <FaCheckCircle /> : <FaExclamationTriangle />}
                  <span>{lsStatus?.found ? `Lossless.dll найден (${lsStatus.size_mb} MB)` : "Lossless.dll не найден!"}</span>
                </div>
                {lsStatus?.found ? (
                  <div style={{ color: "#aaa", fontSize: "11px", wordBreak: "break-all" }}>
                    Путь: {lsStatus.path}<br/>
                    Режим: LSFG 2.3 FP16 (Ветка Steam: lsfg-vk)
                  </div>
                ) : (
                  <div style={{ color: "#fca5a5", fontSize: "11px" }}>
                    Установите Lossless Scaling в Steam и переключитесь на бета-ветку:<br/>
                    <b>Свойства игры → Бета-версии → lsfg-vk</b>
                  </div>
                )}
              </div>
            </PanelSectionRow>

            <PanelSectionRow>
              <DropdownItem
                label="Множитель кадров"
                description="Количество генерируемых кадров"
                menuLabel="Множитель"
                rgOptions={[
                  { label: "2x (30 → 60 FPS / 45 → 90 FPS)", data: 2 },
                  { label: "3x (30 → 90 FPS)", data: 3 },
                  { label: "4x (30 → 120 FPS)", data: 4 }
                ]}
                selectedOption={multiplier}
                onChange={handleMultiplierChange}
              />
            </PanelSectionRow>

            <PanelSectionRow>
              <ToggleField
                label="FP16 Rapid Packed Math"
                description="Аппаратное 2x ускорение на AMD RDNA2 GPU Steam Deck (~4 мс/кадр)"
                checked={performanceMode}
                onChange={handlePerformanceModeChange}
              />
            </PanelSectionRow>
          </>
        )}

        <PanelSectionRow>
          <ToggleField
            label="Глобальный режим (без параметров)"
            description="Слой активен во ВСЕХ играх без указания параметров запуска в Steam"
            checked={globalMode}
            onChange={handleGlobalToggle}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <SliderField
            label="Разрешение векторов (Flow Scale)"
            description="Плотность расчёта движения (90% по умолчанию)"
            value={flowScale}
            min={50}
            max={100}
            step={5}
            showValue={true}
            valueSuffix="%"
            onChange={handleFlowScaleChange}
          />
        </PanelSectionRow>

        {engine === "skyframe" && (
          <>
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
          </>
        )}

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
                <span style={{ color: "#4ade80", fontWeight: "bold" }}>
                  ● Активно ({engine === "lsfg" ? `LSFG ${multiplier}x [FP16]` : "DIS-Flow + TV-L1"})
                </span>
              </div>
              <div style={{ display: "flex", justifyContent: "space-between" }}>
                <span style={{ color: "#aaa" }}>Частота кадров:</span>
                <span style={{ color: "#f59e0b", fontWeight: "bold" }}>
                  {baseFps} FPS → {outputFps} FPS ({targetHz} Hz Auto)
                </span>
              </div>
              <div style={{ display: "flex", justifyContent: "space-between" }}>
                <span style={{ color: "#aaa" }}>Движок:</span>
                <span style={{ color: "#4ade80", fontWeight: "bold" }}>
                  {engine === "lsfg" ? "Lossless Scaling (SPIR-V Compute Shaders)" : "DIS-Flow Optical (Vulkan Compute)"}
                </span>
              </div>
              {status && (
                <div style={{ display: "flex", justifyContent: "space-between", borderTop: "1px solid rgba(255,255,255,0.1)", paddingTop: "4px" }}>
                  <span style={{ color: "#aaa" }}>Библиотеки слоя:</span>
                  <span style={{ color: (engine === "lsfg" ? status.lib64_lsfg : status.lib64_sky) ? "#4ade80" : "#f87171" }}>
                    64-bit: {(engine === "lsfg" ? status.lib64_lsfg : status.lib64_sky) ? "✓" : "✗"} | 
                    32-bit: {(engine === "lsfg" ? status.lib32_lsfg : status.lib32_sky) ? "✓" : "✗"}
                  </span>
                </div>
              )}
            </div>
          </PanelSectionRow>
        )}
      </PanelSection>

      <PanelSection title="Параметры запуска Steam">
        {globalMode ? (
          <PanelSectionRow>
            <div style={{ color: "#4ade80", fontSize: "12px", padding: "4px" }}>
              ✓ Глобальный режим включен! Слой автоматически инжектируется во все игры без параметров.
            </div>
          </PanelSectionRow>
        ) : (
          <>
            <PanelSectionRow>
              <ButtonItem
                layout="below"
                onClick={() => copyToClipboard(engine === "lsfg" ? "ENABLE_LSFGVK=1 %command%" : "ENABLE_SKYFRAME=1 %command%", "Команда запуска")}
              >
                📋 Скопировать {engine === "lsfg" ? "ENABLE_LSFGVK=1 %command%" : "ENABLE_SKYFRAME=1 %command%"}
              </ButtonItem>
            </PanelSectionRow>

            <PanelSectionRow>
              <ButtonItem
                layout="below"
                onClick={() => copyToClipboard(
                  engine === "lsfg" 
                    ? "VK_LOADER_DEBUG=all ENABLE_LSFGVK=1 %command% > ~/lsfg_game.log 2>&1" 
                    : "VK_LOADER_DEBUG=all ENABLE_SKYFRAME=1 %command% > ~/skyframe_game.log 2>&1", 
                  "Диагностический запуск"
                )}
              >
                🛠️ Скопировать запуск с подробным логом Vulkan
              </ButtonItem>
            </PanelSectionRow>
          </>
        )}

        {engine === "lsfg" && (
          <PanelSectionRow>
            <ButtonItem
              layout="below"
              onClick={() => copyToClipboard("lsfg-vk-cli healthcheck", "Команда проверки системы")}
            >
              🩺 Скопировать команду диагностики (lsfg-vk-cli healthcheck)
            </ButtonItem>
          </PanelSectionRow>
        )}
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
