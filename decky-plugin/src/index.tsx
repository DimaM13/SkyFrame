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
  global_injection?: boolean;
  multiplier?: number;
  performance_mode?: boolean;
  target_hz?: number;
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
  lib64: boolean;
  lib32: boolean;
  enabled: boolean;
  global_mode: boolean;
  ls_info: LosslessStatus;
}

const getConfig = callable<[], SkyFrameConfig>("get_config");
const setEnabledCall = callable<[enabled: boolean], SkyFrameConfig>("set_enabled");
const setMultiplierCall = callable<[multiplier: number], SkyFrameConfig>("set_multiplier");
const setPerformanceModeCall = callable<[performance_mode: boolean], SkyFrameConfig>("set_performance_mode");
const setGlobalInjectionCall = callable<[global_injection: boolean], SkyFrameConfig>("set_global_injection");
const setTargetHzCall = callable<[target_hz: number], SkyFrameConfig>("set_target_hz");
const setFlowScaleCall = callable<[flow_scale: number], SkyFrameConfig>("set_flow_scale");
const copyToClipboardCall = callable<[text: string], boolean>("copy_to_clipboard");
const getStatsCall = callable<[], { base_fps: number; output_fps: number; target_hz?: number }>("get_stats");
const getStatusInfoCall = callable<[], StatusInfo>("get_status_info");
const getLosslessStatusCall = callable<[], LosslessStatus>("get_lossless_status");

function Content() {
  const [enabled, setEnabled] = useState<boolean>(false);
  const [globalMode, setGlobalMode] = useState<boolean>(false);
  const [multiplier, setMultiplier] = useState<number>(2);
  const [performanceMode, setPerformanceMode] = useState<boolean>(true);
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
        setGlobalMode(Boolean(res.global_injection ?? false));
        setMultiplier(Number(res.multiplier ?? 2));
        setPerformanceMode(Boolean(res.performance_mode ?? true));
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
      <PanelSection title="SkyFrame: Lossless Scaling (LSFG)">
        <PanelSectionRow>
          <ToggleField
            label="Генерация кадров (LSFG)"
            description="Нативная нейросеть Lossless Scaling (24 SPIR-V прохода на GPU)"
            checked={enabled}
            onChange={handleToggle}
          />
        </PanelSectionRow>

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
              <span>{lsStatus?.found ? `Lossless.dll обнаружен (${lsStatus.size_mb} MB)` : "Lossless.dll не найден!"}</span>
            </div>
            {lsStatus?.found ? (
              <div style={{ color: "#aaa", fontSize: "11px", wordBreak: "break-all" }}>
                Путь: {lsStatus.path}<br/>
                Модель: LSFG 2.3 FP16 (Ветка Steam: lsfg-vk)
              </div>
            ) : (
              <div style={{ color: "#fca5a5", fontSize: "11px" }}>
                Установите Lossless Scaling в Steam и выберите бета-ветку:<br/>
                <b>Свойства игры → Бета-версии → lsfg-vk</b>
              </div>
            )}
          </div>
        </PanelSectionRow>

        <PanelSectionRow>
          <DropdownItem
            label="Множитель кадров"
            description="Целевое умножение частоты кадров"
            menuLabel="Множитель"
            rgOptions={[
              { label: "2x (30 → 60 FPS / 45 → 90 FPS)", data: 2 },
              { label: "3x (30 → 90 FPS на OLED)", data: 3 },
              { label: "4x (30 → 120 FPS)", data: 4 }
            ]}
            selectedOption={multiplier}
            onChange={handleMultiplierChange}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <ToggleField
            label="FP16 Rapid Packed Math"
            description="Аппаратное 2x ускорение на AMD RDNA2 GPU Steam Deck (~4 мс на кадр)"
            checked={performanceMode}
            onChange={handlePerformanceModeChange}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <ToggleField
            label="Глобальный режим (без параметров)"
            description="Слой активен во ВСЕХ играх без прописывания параметров запуска в Steam"
            checked={globalMode}
            onChange={handleGlobalToggle}
          />
        </PanelSectionRow>

        <PanelSectionRow>
          <SliderField
            label="Разрешение потока (Flow Scale)"
            description="Плотность расчёта векторов движения (90% рекомендуется)"
            value={flowScale}
            min={50}
            max={100}
            step={5}
            showValue={true}
            valueSuffix="%"
            onChange={handleFlowScaleChange}
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
                <span style={{ color: "#4ade80", fontWeight: "bold" }}>
                  ● Активно (LSFG {multiplier}x {performanceMode ? "[FP16]" : "[FP32]"})
                </span>
              </div>
              <div style={{ display: "flex", justifyContent: "space-between" }}>
                <span style={{ color: "#aaa" }}>Частота кадров:</span>
                <span style={{ color: "#f59e0b", fontWeight: "bold" }}>
                  {baseFps} FPS → {outputFps} FPS ({targetHz} Hz Экран)
                </span>
              </div>
              <div style={{ display: "flex", justifyContent: "space-between" }}>
                <span style={{ color: "#aaa" }}>Движок:</span>
                <span style={{ color: "#4ade80", fontWeight: "bold" }}>Lossless Scaling (Vulkan SPIR-V Compute)</span>
              </div>
              {status && (
                <div style={{ display: "flex", justifyContent: "space-between", borderTop: "1px solid rgba(255,255,255,0.1)", paddingTop: "4px" }}>
                  <span style={{ color: "#aaa" }}>Библиотеки слоя:</span>
                  <span style={{ color: status.lib64 ? "#4ade80" : "#f87171" }}>
                    64-bit: {status.lib64 ? "✓" : "✗"} | 32-bit: {status.lib32 ? "✓" : "✗"}
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
              ✓ Глобальный режим включен! LSFG автоматически инжектируется во все игры без параметров.
            </div>
          </PanelSectionRow>
        ) : (
          <>
            <PanelSectionRow>
              <ButtonItem
                layout="below"
                onClick={() => copyToClipboard("ENABLE_LSFGVK=1 %command%", "Команда запуска")}
              >
                📋 Скопировать ENABLE_LSFGVK=1 %command%
              </ButtonItem>
            </PanelSectionRow>

            <PanelSectionRow>
              <ButtonItem
                layout="below"
                onClick={() => copyToClipboard("VK_LOADER_DEBUG=all ENABLE_LSFGVK=1 %command% > ~/lsfg_game.log 2>&1", "Диагностический запуск")}
              >
                🛠️ Скопировать запуск с подробным логом Vulkan
              </ButtonItem>
            </PanelSectionRow>
          </>
        )}

        <PanelSectionRow>
          <ButtonItem
            layout="below"
            onClick={() => copyToClipboard("lsfg-vk-cli healthcheck", "Команда проверки системы")}
          >
            🩺 Скопировать команду диагностики (lsfg-vk-cli healthcheck)
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
