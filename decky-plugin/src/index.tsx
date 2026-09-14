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
import { FaBolt, FaCheckCircle, FaExclamationTriangle, FaRocket, FaHeartbeat } from "react-icons/fa";

interface SkyFrameConfig {
  enabled: boolean;
  global_injection?: boolean;
  frame_generation_enabled?: boolean;
  multiplier?: number;
  adaptive?: boolean;
  target_fps?: number;
  adaptive_max_multiplier?: number;
  performance_mode?: boolean;
  target_hz?: number;
  flow_scale?: number;
  pacing?: string;
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
  shim64?: boolean;
  shim32?: boolean;
  runner: boolean;
  enabled: boolean;
  global_mode: boolean;
  ls_info: LosslessStatus;
}

interface Stats {
  base_fps: number;
  output_fps: number;
  gen_ms?: number;
  jitter_ms?: number;
  avg_interval_ms?: number;
  drops?: number;
  queued?: number;
  present_mode?: string;
  live?: boolean;
  source?: string;
  target_hz?: number;
}

const getConfig = callable<[], SkyFrameConfig>("get_config");
const setEnabledCall = callable<[enabled: boolean], SkyFrameConfig>("set_enabled");
const setFgOnCall = callable<[v: boolean], SkyFrameConfig>("set_frame_generation_enabled");
const setMultiplierCall = callable<[multiplier: number], SkyFrameConfig>("set_multiplier");
const setAdaptiveCall = callable<[v: boolean], SkyFrameConfig>("set_adaptive");
const setTargetFpsCall = callable<[v: number], SkyFrameConfig>("set_target_fps");
const setAdaptMaxCall = callable<[v: number], SkyFrameConfig>("set_adaptive_max_multiplier");
const setPerformanceModeCall = callable<[performance_mode: boolean], SkyFrameConfig>("set_performance_mode");
const setGlobalInjectionCall = callable<[global_injection: boolean], SkyFrameConfig>("set_global_injection");
const setTargetHzCall = callable<[target_hz: number], SkyFrameConfig>("set_target_hz");
const setFlowScaleCall = callable<[flow_scale: number], SkyFrameConfig>("set_flow_scale");
const setPacingCall = callable<[pacing: string], SkyFrameConfig>("set_pacing");
const copyToClipboardCall = callable<[text: string], boolean>("copy_to_clipboard");
const getStatsCall = callable<[], Stats>("get_stats");
const getStatusInfoCall = callable<[], StatusInfo>("get_status_info");
const getLosslessStatusCall = callable<[], LosslessStatus>("get_lossless_status");

const card: React.CSSProperties = {
  background: "rgba(255,255,255,0.05)",
  border: "1px solid rgba(255,255,255,0.09)",
  borderRadius: "10px",
  padding: "10px",
  fontSize: "12px",
  display: "flex",
  flexDirection: "column",
  gap: "8px"
};

function Content() {
  const [enabled, setEnabled] = useState<boolean>(false);
  const [fgOn, setFgOn] = useState<boolean>(true);
  const [globalMode, setGlobalMode] = useState<boolean>(false);
  const [multiplier, setMultiplier] = useState<number>(2);
  const [adaptive, setAdaptive] = useState<boolean>(false);
  const [targetFps, setTargetFps] = useState<number>(90);
  const [adaptMax, setAdaptMax] = useState<number>(3);
  const [performanceMode, setPerformanceMode] = useState<boolean>(true);
  const [targetHz, setTargetHz] = useState<number>(60);
  const [flowScale, setFlowScale] = useState<number>(90);
  const [pacing, setPacing] = useState<string>("smooth");
  const [stats, setStats] = useState<Stats>({ base_fps: 0, output_fps: 0 });
  const [status, setStatus] = useState<StatusInfo | null>(null);
  const [lsStatus, setLsStatus] = useState<LosslessStatus | null>(null);

  useEffect(() => {
    getConfig().then((res) => {
      if (res) {
        setEnabled(Boolean(res.enabled));
        setFgOn(res.frame_generation_enabled ?? true);
        setGlobalMode(Boolean(res.global_injection ?? false));
        setMultiplier(Number(res.multiplier ?? 2));
        setAdaptive(Boolean(res.adaptive ?? false));
        setTargetFps(Number(res.target_fps ?? 90));
        setAdaptMax(Number(res.adaptive_max_multiplier ?? 3));
        setPerformanceMode(Boolean(res.performance_mode ?? true));
        if (res.target_hz) setTargetHz(Number(res.target_hz));
        if (res.flow_scale !== undefined) setFlowScale(Math.round(Number(res.flow_scale) * 100));
        if (res.pacing) setPacing(String(res.pacing));
      }
    }).catch((e) => console.error("[SkyFrame] config:", e));

    const loadStatus = () => {
      getStatusInfoCall().then((s) => {
        setStatus(s);
        if (s?.ls_info) setLsStatus(s.ls_info);
      }).catch(() => {});
      getLosslessStatusCall().then(setLsStatus).catch(() => {});
    };
    loadStatus();

    const interval = setInterval(() => {
      getStatsCall().then((st) => {
        if (st) {
          setStats(st);
          if (st.target_hz) setTargetHz(Math.round(st.target_hz));
        }
      }).catch(() => {});
      loadStatus();
    }, 1500);
    return () => clearInterval(interval);
  }, []);

  const refreshStatus = () => setTimeout(() => getStatusInfoCall().then(setStatus).catch(() => {}), 600);

  const copyCmd = (cmd: string, desc: string) => {
    try {
      (window as any).SteamClient?.System?.SetClipboardText?.(cmd);
      navigator.clipboard?.writeText?.(cmd).catch(() => {});
      const ta = document.createElement("textarea");
      ta.value = cmd;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand("copy");
      document.body.removeChild(ta);
      copyToClipboardCall(cmd).catch(() => {});
      toaster.toast({ title: "SkyFrame", body: `Скопировано: ${desc}` });
    } catch (e) { console.error(e); }
  };

  const pacingOpt = pacing === "smooth" ? 1 : pacing === "none" ? 2 : 0;
  const effMult = adaptive ? `Adaptive→${targetFps} (max ${adaptMax}x)` : `${multiplier}x`;
  const hudColor = !enabled ? "#888" : stats.live ? "#4ade80" : "#f59e0b";

  return (
    <>
      <PanelSection title="SkyFrame v2 ・ Frame Generation">
        {/* STATUS HEADER */}
        <PanelSectionRow>
          <div style={{
            ...card,
            background: "linear-gradient(135deg, rgba(56,189,248,0.14), rgba(168,85,247,0.14))",
            border: "1px solid rgba(56,189,248,0.35)"
          }}>
            <div style={{ display: "flex", alignItems: "center", gap: 8, fontWeight: 700, fontSize: 14 }}>
              <FaRocket color="#38bdf8" />
              <span>SkyFrame {enabled && fgOn ? "● ACTIVE" : "○ PAUSED"}</span>
              <span style={{ marginLeft: "auto", fontSize: 11, color: hudColor, fontWeight: 700 }}>
                {Math.round(stats.base_fps)} → {Math.round(stats.output_fps)} FPS
                {stats.live ? " ・LIVE" : " ・est"}
              </span>
            </div>
            <div style={{ display: "flex", gap: 12, color: "#aaa", fontSize: 11 }}>
              <span>Режим: <b style={{ color: "#fff" }}>{effMult}{performanceMode ? " [FP16]" : ""}</b></span>
              <span>Экран: <b style={{ color: "#fff" }}>{targetHz} Hz</b></span>
              {(stats.gen_ms ?? 0) > 0 && <span>Gen: <b style={{ color: "#fff" }}>{stats.gen_ms!.toFixed(1)} ms</b></span>}
            {(stats.jitter_ms ?? 0) > 0 && <span>Jitter: <b style={{ color: "#fff" }}>{stats.jitter_ms!.toFixed(1)} ms</b></span>}
            </div>
            {(stats.live) && (
              <div style={{ display: "flex", gap: 12, color: "#aaa", fontSize: 11 }}>
                <span>Дропы: <b style={{ color: "#fff" }}>{stats.drops ?? 0}</b></span>
                <span>Очередь: <b style={{ color: "#fff" }}>{stats.queued ?? 0}</b></span>
                {stats.present_mode && <span>Mode: <b style={{ color: "#fff" }}>{stats.present_mode}</b></span>}
              </div>
            )}
            <div style={{ height: 6, borderRadius: 4, background: "rgba(255,255,255,0.1)", overflow: "hidden" }}>
              <div style={{
                width: `${Math.min(100, (stats.output_fps / Math.max(1, targetHz)) * 100)}%`,
                height: "100%",
                background: hudColor,
                transition: "width .4s"
              }} />
            </div>
            {status && (
              <div style={{ display: "flex", gap: 8, fontSize: 11, color: "#aaa" }}>
                <span>Ядро: <b style={{ color: status.lib64 ? "#4ade80" : "#f87171" }}>64{status.lib64 ? "✓" : "✗"}</b> / <b style={{ color: status.lib32 ? "#4ade80" : "#f87171" }}>32{status.lib32 ? "✓" : "✗"}</b></span>
                <span>Шим: <b style={{ color: status.shim64 ? "#4ade80" : "#f87171" }}>64{status.shim64 ? "✓" : "✗"}</b> / <b style={{ color: status.shim32 ? "#4ade80" : "#f87171" }}>32{status.shim32 ? "✓" : "✗"}</b></span>
                <span>Раннер: <b style={{ color: status.runner ? "#4ade80" : "#f87171" }}>{status.runner ? "skyframe-run ✓" : "нет ✗"}</b></span>
              </div>
            )}
          </div>
        </PanelSectionRow>

        {/* MASTER SWITCHES */}
        <PanelSectionRow>
          <ToggleField
            label="SkyFrame включен"
            description={globalMode ? "Глобальный режим: слой во всех играх (применение — с рестарта игры)" : "Per-game: запускай игру через skyframe-run (вкл/выкл — до запуска игры)"}
            checked={enabled}
            onChange={(v) => { setEnabled(v); setEnabledCall(v).catch(console.error); refreshStatus(); }}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <ToggleField
            label="Live-toggle генерации ⚠️"
            description="НЕ трогать в запущенной игре — офиц. ядро 2.0.0 крашится на пересоздании контекста. Меняй до запуска. Настоящий live будет в своем слое."
            checked={fgOn}
            onChange={(v) => { setFgOn(v); setFgOnCall(v).catch(console.error); }}
          />
        </PanelSectionRow>

        {/* DLL STATUS */}
        <PanelSectionRow>
          <div style={{
            ...card,
            background: lsStatus?.found ? "rgba(34,197,94,0.10)" : "rgba(239,68,68,0.10)",
            border: `1px solid ${lsStatus?.found ? "#22c55e55" : "#ef444455"}`
          }}>
            <div style={{ display: "flex", alignItems: "center", gap: 6, fontWeight: 700, color: lsStatus?.found ? "#4ade80" : "#f87171" }}>
              {lsStatus?.found ? <FaCheckCircle /> : <FaExclamationTriangle />}
              <span>{lsStatus?.found ? `Lossless.dll найден (${lsStatus.size_mb} MB)` : "Lossless.dll не найден"}</span>
            </div>
            <div style={{ color: "#aaa", fontSize: 11, wordBreak: "break-all" }}>
              {lsStatus?.found ? (<>{lsStatus.path}<br />Ветка Steam: <b>lsfg-vk</b> (нужна lsfg-vk.dll внутри)</>) : (
                <>Установи Lossless Scaling → Свойства → Бета-версии → <b>lsfg-vk</b></>
              )}
            </div>
          </div>
        </PanelSectionRow>

        {/* FIXED vs ADAPTIVE */}
        <PanelSectionRow>
          <ToggleField
            label="Adaptive FG (скоро)"
            description={`Движком пока игнорируется (настройка едет в skyframe.toml для будущего слоя). Сейчас работает только фикс ${multiplier}x`}
            checked={adaptive}
            onChange={(v) => { setAdaptive(v); setAdaptiveCall(v).catch(console.error); }}
          />
        </PanelSectionRow>
        {!adaptive ? (
          <PanelSectionRow>
            <DropdownItem
              label="Множитель (Fixed)"
              description="30→60 / 30→90 OLED / 45→90"
              menuLabel="Множитель"
              rgOptions={[
                { label: "2x — стабильно", data: 2 },
                { label: "3x — OLED 90", data: 3 },
                { label: "4x — 120Hz док", data: 4 }
              ]}
              selectedOption={multiplier}
              onChange={(it: any) => { const v = Number(it.data); setMultiplier(v); setMultiplierCall(v).catch(console.error); }}
            />
          </PanelSectionRow>
        ) : (
          <>
            <PanelSectionRow>
              <SliderField label="Adaptive target" description="Целевой FPS" value={targetFps} min={30} max={240} step={5} showValue valueSuffix=" FPS"
                onChange={(v) => { setTargetFps(v); setTargetFpsCall(v).catch(console.error); }} />
            </PanelSectionRow>
            <PanelSectionRow>
              <DropdownItem label="Потолок Adaptive" description="Макс. генераций на кадр" menuLabel="Max"
                rgOptions={[{ label: "2x", data: 2 }, { label: "3x", data: 3 }, { label: "4x", data: 4 }]}
                selectedOption={adaptMax}
                onChange={(it: any) => { const v = Number(it.data); setAdaptMax(v); setAdaptMaxCall(v).catch(console.error); }} />
            </PanelSectionRow>
          </>
        )}

        <PanelSectionRow>
          <ToggleField label="FP16 Rapid Packed Math" description="2-3x быстрее на RDNA2 Deck. Выкл только для старых NVIDIA"
            checked={performanceMode} onChange={(v) => { setPerformanceMode(v); setPerformanceModeCall(v).catch(console.error); }} />
        </PanelSectionRow>
        <PanelSectionRow>
          <SliderField label="Flow Scale" description="Плотность векторов. 90% = баланс Deck" value={flowScale} min={50} max={100} step={5} showValue valueSuffix="%"
            onChange={(v) => { setFlowScale(v); setFlowScaleCall(v / 100).catch(console.error); }} />
        </PanelSectionRow>
        <PanelSectionRow>
          <DropdownItem label="Доставка кадров" description="Smooth=гладко(+лаг) Balanced=дефолт Low-lag=мин.лаг" menuLabel="Pacing"
            rgOptions={[{ label: "Smooth (как 2.0.0)", data: 0 }, { label: "Balanced — очередь 1 + дедлайн (дефолт)", data: 1 }, { label: "Low-lag (mailbox)", data: 2 }]}
            selectedOption={pacingOpt}
            onChange={(it: any) => {
              const m = ["vsync", "smooth", "none"][Number(it.data)];
              setPacing(m); setPacingCall(m).catch(console.error);
            }} />
        </PanelSectionRow>
        <PanelSectionRow>
          <ToggleField label="Глобальный режим" description="Без параметров запуска. Иначе только через skyframe-run"
            checked={globalMode} onChange={(v) => { setGlobalMode(v); setGlobalInjectionCall(v).catch(console.error); refreshStatus(); }} />
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title="Запуск игры">
        {globalMode ? (
          <PanelSectionRow>
            <div style={{ ...card, borderColor: "#22c55e55", color: "#4ade80" }}>
              <FaHeartbeat /> Глобальный режим: просто запускай игру, слой уже везде.
            </div>
          </PanelSectionRow>
        ) : (
          <>
            <PanelSectionRow>
              <ButtonItem layout="below" onClick={() => copyCmd(`${status ? (status.user_home + "/.local/bin/skyframe-run") : "~/.local/bin/skyframe-run"} %command%`, "skyframe-run")}>
                📋 Скопировать: skyframe-run %command%
              </ButtonItem>
            </PanelSectionRow>
            <PanelSectionRow>
              <ButtonItem layout="below" onClick={() => copyCmd(`${status ? (status.user_home + "/.local/bin/skyframe-run") : "~/.local/bin/skyframe-run"} --debug %command%`, "debug-запуск")}>
                🛠️ Скопировать debug-запуск (лог ~/skyframe_game.log)
              </ButtonItem>
            </PanelSectionRow>
          </>
        )}
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => copyCmd("lsfg-vk-cli healthcheck && lsfg-vk-cli validate", "диагностика")}>
            🩺 Скопировать диагностику (healthcheck + validate)
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => copyCmd("lsfg-vk-cli benchmark -w 1280 -h 800 -m 2", "бенчмарк")}>
            <FaBolt /> Скопировать бенчмарк Deck (1280x800 2x)
          </ButtonItem>
        </PanelSectionRow>
      </PanelSection>
    </>
  );
}

export default definePlugin(() => ({
  name: "SkyFrame",
  titleView: <div className={staticClasses.Title}>SkyFrame v2</div>,
  content: <Content />,
  icon: <FaBolt />,
  onDismount() {}
}));
