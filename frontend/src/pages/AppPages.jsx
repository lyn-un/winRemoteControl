import React, { useEffect, useMemo, useRef, useState } from "react";
import { sendCommand, sendPreviewRect } from "../bridge/nativeBridge";
import CaptureStatus from "../components/CaptureStatus";
import DesktopWindowControls from "../components/DesktopWindowControls";
import SessionMetrics from "../components/SessionMetrics";
import { useNativeState } from "../state/useNativeState";

export function DashboardPage() {
  const state = useNativeState();
  const numericPort = Number.parseInt(state.port, 10) || 39000;
  const remoteOnline = state.role === "controller" && state.sessionOpen;
  const computerName = state.deviceInfo?.computerName || "远程电脑";
  const wallpaperUrl = state.deviceInfo?.wallpaperData
    ? `data:${state.deviceInfo.wallpaperMime || "image/jpeg"};base64,${state.deviceInfo.wallpaperData}`
    : "";
  const screenWidth = state.deviceInfo?.screenWidth || state.frame?.width || 16;
  const screenHeight = state.deviceInfo?.screenHeight || state.frame?.height || 9;
  const desktopEntryStyle = {
    aspectRatio: `${screenWidth} / ${screenHeight}`,
    ...(wallpaperUrl
      ? { backgroundImage: `linear-gradient(90deg, rgba(10,18,24,.35), rgba(10,18,24,.08)), url(${wallpaperUrl})` }
      : {}),
  };

  return (
    <main className="app-shell dashboard-shell">
      <aside className="sidebar">
        <div className="brand">
          <span className="mark">WRC</span>
          <div>
            <h1>winRemoteControl</h1>
            <p>局域网远程桌面验证</p>
          </div>
        </div>

        <section className="connect-panel">
          <div className="section-title">
            <h2>会话</h2>
            <p>手动连接局域网设备</p>
          </div>

          <div className="segmented">
            <button className={state.role === "controlled" ? "active" : ""} onClick={() => state.setRole("controlled")}>
              被控端
            </button>
            <button className={state.role === "controller" ? "active" : ""} onClick={() => state.setRole("controller")}>
              控制端
            </button>
          </div>

          {state.role === "controller" && (
            <label className="field">
              <span>被控端 IP</span>
              <input value={state.host} onChange={(event) => state.setHost(event.target.value)} />
            </label>
          )}

          <label className="field">
            <span>{state.role === "controlled" ? "监听端口" : "被控端端口"}</span>
            <input value={state.port} onChange={(event) => state.setPort(event.target.value)} />
          </label>

          <div className="actions">
            {state.role === "controlled" ? (
              <button className="primary" onClick={() => sendCommand("startSignalingServer", { port: numericPort })}>
                开始监听
              </button>
            ) : (
              <button className="primary" onClick={() => sendCommand("connectSignaling", { host: state.host, port: numericPort })}>
                连接被控端
              </button>
            )}
            <button className="secondary" onClick={() => sendCommand("disconnectSession")}>
              断开会话
            </button>
          </div>

          <SessionMetrics
            signalingState={state.signalingState}
            webrtcState={state.webrtcState}
            sessionOpen={state.sessionOpen}
          />

          {state.error && <p className="error">{state.error}</p>}
          {state.lanDiscoveryError && <p className="error">{state.lanDiscoveryError}</p>}
        </section>
      </aside>

      <section className="device-page">
        <header className="page-head">
          <div>
            <h2>{state.role === "controlled" ? "本机等待控制" : "我的设备"}</h2>
            <p>{state.role === "controlled" ? "保持此窗口可见，等待控制端连接。" : "连接成功后，从设备卡片进入远程桌面。"}</p>
          </div>
          <div className="page-head-actions">
            {state.role === "controller" && !remoteOnline && (
              <button className="secondary" onClick={() => sendCommand("refreshLanDevices")}>
                刷新设备
              </button>
            )}
            <CaptureStatus captureStatus={state.captureStatus} webrtcState={state.webrtcState} />
          </div>
        </header>

        {state.role === "controlled" ? (
          <div className="controlled-card">
            <strong>{state.sessionOpen ? "控制端已连接" : "正在等待连接"}</strong>
            <p>对方点击“进入桌面”后，本机才会开始推送画面。</p>
            {state.sessionOpen && (
              <button className="secondary danger-button" onClick={() => sendCommand("stopStreaming")}>
                结束控制
              </button>
            )}
          </div>
        ) : (
          <div className="device-list">
            {remoteOnline ? (
              <article className="device-card">
                <div className="device-title">
                  <span className="online-dot" />
                  <h3>{computerName}</h3>
                </div>
                <button
                  className="desktop-entry"
                  style={desktopEntryStyle}
                  onClick={() => sendCommand("enterDesktop")}
                >
                  <span>进入桌面</span>
                  <strong>→</strong>
                </button>
                <div className="device-meta">
                  <span>{state.frame ? `${state.frame.width} × ${state.frame.height}` : "等待画面"}</span>
                  <span>{state.fps} FPS</span>
                </div>
              </article>
            ) : state.lanDevices.length > 0 ? (
              <div className="lan-device-grid">
                {state.lanDevices.map((device) => (
                  <article className="lan-device-card" key={device.deviceId}>
                    <div className="device-title">
                      <span className="online-dot" />
                      <div>
                        <h3>{device.name || "Windows 设备"}</h3>
                        <p>{device.address}:{device.port}</p>
                      </div>
                    </div>
                    <button
                      className="primary"
                      onClick={() => sendCommand("connectLanDevice", { deviceId: device.deviceId })}
                    >
                      连接
                    </button>
                  </article>
                ))}
              </div>
            ) : (
              <div className="empty-device">
                <strong>还没有在线设备</strong>
                <p>正在搜索局域网设备，也可以在左侧输入 IP 和端口手动连接。</p>
              </div>
            )}
          </div>
        )}
      </section>
    </main>
  );
}

export function DesktopPage() {
  const state = useNativeState();
  const previewSlotRef = useRef(null);
  const [elapsedSeconds, setElapsedSeconds] = useState(0);
  const sessionUnavailable = ["Interrupted", "Stopping", "Disconnected", "Failed"]
    .includes(state.webrtcState);

  const qualityPresets = {
    ultraFast: { label: "极速", fps: 60, width: 1280, height: 720, bitrateKbps: 4000 },
    auto: { label: "自动", fps: 30, width: 1280, height: 720, bitrateKbps: 3000 },
    original: { label: "原画", fps: 30, width: 0, height: 0, bitrateKbps: 12000 },
    hd: { label: "高清", fps: 30, width: 1920, height: 1080, bitrateKbps: 6000 },
    smooth: { label: "流畅", fps: 30, width: 1280, height: 720, bitrateKbps: 2000 },
  };

  const sendStreamConfig = (key) => {
    const config = qualityPresets[key] || qualityPresets.auto;
    sendCommand("setStreamConfig", {
      fps: config.fps,
      width: config.width,
      height: config.height,
      bitrateKbps: config.bitrateKbps,
    });
  };

  const formatElapsed = (seconds) => {
    const nHours = Math.floor(seconds / 3600);
    const nMinutes = Math.floor((seconds % 3600) / 60);
    const nSeconds = seconds % 60;
    const pad = (value) => String(value).padStart(2, "0");
    return nHours > 0
      ? `${pad(nHours)}:${pad(nMinutes)}:${pad(nSeconds)}`
      : `${pad(nMinutes)}:${pad(nSeconds)}`;
  };

  const networkClass = useMemo(() => {
    const value = state.webrtcState.toLowerCase();
    const isNetworkActive =
      state.fps > 0 ||
      value.includes("streaming") ||
      value.includes("remotevideotrack") ||
      value.includes("sessionchannelopen");

    if (state.networkStats.quality === "excellent") {
      return "network-dot is-excellent";
    }
    if (state.networkStats.quality === "good") {
      return "network-dot is-good";
    }
    if (state.networkStats.quality === "poor") {
      return "network-dot is-bad";
    }

    if (value.includes("failed") || value.includes("disconnect")) {
      return "network-dot is-bad";
    }
    if (value.includes("checking") || value.includes("connecting")) {
      return "network-dot is-warn";
    }
    if (isNetworkActive) {
      return "network-dot is-warn";
    }
    if (value.includes("connected") || value.includes("completed") || value.includes("streaming")) {
      return "network-dot is-good";
    }
    return "network-dot";
  }, [state.fps, state.networkStats.quality, state.webrtcState]);

  const networkLabel = useMemo(() => {
    const stats = state.networkStats;
    if (!stats || stats.quality === "unknown" || stats.rttMs < 0) {
      if (state.fps > 0) {
        return "统计中";
      }
      return "";
    }

    return `${stats.rttMs}ms`;
  }, [state.fps, state.networkStats]);

  const networkTitle = useMemo(() => {
    const stats = state.networkStats;
    if (!stats || stats.quality === "unknown" || stats.rttMs < 0) {
      if (state.fps > 0) {
        return "网络统计采集中";
      }
      return "网络状态未知";
    }

    const lossPercent = (stats.packetLossRate * 100).toFixed(1);
    const bitrateMbps = (stats.bitrateKbps / 1000).toFixed(1);
    return `网络 ${stats.quality} / RTT ${stats.rttMs}ms / 抖动 ${stats.jitterMs}ms / 丢包 ${lossPercent}% / ${bitrateMbps}Mbps`;
  }, [state.networkStats]);

  useEffect(() => {
    if (!previewSlotRef.current) {
      return undefined;
    }

    const element = previewSlotRef.current;
    const notify = () => sendPreviewRect(element);
    notify();

    const resizeObserver = new ResizeObserver(notify);
    resizeObserver.observe(element);
    window.addEventListener("resize", notify);

    const timerId = window.setTimeout(notify, 100);
    return () => {
      window.clearTimeout(timerId);
      resizeObserver.disconnect();
      window.removeEventListener("resize", notify);
    };
  }, []);

  useEffect(() => {
    const timerId = window.setInterval(() => {
      setElapsedSeconds((value) => value + 1);
    }, 1000);
    return () => window.clearInterval(timerId);
  }, []);

  useEffect(() => {
    sendStreamConfig("auto");
  }, []);

  return (
    <main className="desktop-shell">
      <header
        className="desktop-titlebar"
        onPointerDown={(event) => {
          if (event.button === 0 && event.target.closest("[data-window-control]") === null) {
            sendCommand("beginDesktopWindowDrag");
          }
        }}
      >
        <div className="desktop-title-left">
          <span className="mark">WRC</span>
          <strong>{state.deviceInfo?.computerName || "远程桌面"}</strong>
          <span className="network-status">
            <i className={networkClass} title={networkTitle} />
            {networkLabel && <span className="network-latency">{networkLabel}</span>}
            {formatElapsed(elapsedSeconds)}
          </span>
          <span className="desktop-stat">
            {state.frame ? `${state.frame.width} x ${state.frame.height}` : "等待画面"}
          </span>
          <span className="desktop-stat">{state.fps} FPS</span>
        </div>
        <DesktopWindowControls />
      </header>
      <section className="desktop-stage">
        <div ref={previewSlotRef} className="native-preview-slot desktop-slot" />
        {sessionUnavailable && (
          <div className="desktop-disconnected">
            <strong>{state.webrtcState === "Interrupted" ? "连接暂时中断" : "远程连接已断开"}</strong>
            <p>
              {state.webrtcState === "Interrupted"
                ? "正在等待网络恢复，恢复前不会继续发送输入。"
                : "本次远程控制已经结束，请返回主界面重新连接。"}
            </p>
            {state.webrtcState !== "Interrupted" && (
              <button className="primary" onClick={() => sendCommand("closeDesktop")}>返回主界面</button>
            )}
          </div>
        )}
      </section>
      {state.error && <p className="desktop-error">{state.error}</p>}
    </main>
  );
}
