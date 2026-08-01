import React, { useEffect, useMemo, useRef, useState } from "react";
import { sendCommand, sendPreviewRect } from "../bridge/nativeBridge";
import DesktopWindowControls from "../components/DesktopWindowControls";
import { useNativeState } from "../state/useNativeState";

function Icon({ name }) {
  const paths = {
    devices: <><rect x="3" y="4" width="18" height="13" rx="2" /><path d="M8 21h8M12 17v4" /></>,
    assist: <><circle cx="6" cy="12" r="3" /><circle cx="18" cy="6" r="3" /><circle cx="18" cy="18" r="3" /><path d="m9 11 6-4M9 13l6 4" /></>,
    refresh: <><path d="M20 6v5h-5M4 18v-5h5" /><path d="M6.1 9a7 7 0 0 1 11.5-2.6L20 9M4 15l2.4 2.6A7 7 0 0 0 17.9 15" /></>,
    monitor: <><rect x="3" y="4" width="18" height="14" rx="2" /><path d="M9 22h6M12 18v4" /></>,
    trash: <><path d="M4 7h16M9 7V4h6v3M7 7l1 14h8l1-14M10 11v6M14 11v6" /></>,
    arrow: <><path d="M5 12h14M14 7l5 5-5 5" /></>,
    signal: <><path d="M5 15a10 10 0 0 1 14 0M8 18a6 6 0 0 1 8 0" /><circle cx="12" cy="21" r="1" /></>,
    settings: <><circle cx="12" cy="12" r="3" /><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.83 2.83-.06-.06A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 .6 1.7 1.7 0 0 0-.4 1.1V21h-4v-.09A1.7 1.7 0 0 0 8.6 19.4a1.7 1.7 0 0 0-1.88.34l-.06.06-2.83-2.83.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-.6-1 1.7 1.7 0 0 0-1.1-.4H3v-4h.09A1.7 1.7 0 0 0 4.6 8.6a1.7 1.7 0 0 0-.34-1.88l-.06-.06 2.83-2.83.06.06A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-.6 1.7 1.7 0 0 0 .4-1.1V3h4v.09A1.7 1.7 0 0 0 15.4 4.6a1.7 1.7 0 0 0 1.88-.34l.06-.06 2.83 2.83-.06.06A1.7 1.7 0 0 0 19.4 9c.14.37.35.7.6 1 .3.28.68.42 1.1.4h.09v4h-.09A1.7 1.7 0 0 0 19.4 15Z" /></>,
    shield: <><path d="M12 3 5 6v5c0 4.6 2.8 8.2 7 10 4.2-1.8 7-5.4 7-10V6l-7-3Z" /><path d="m9 12 2 2 4-4" /></>,
  };
  return <svg className="ui-icon" viewBox="0 0 24 24" aria-hidden="true">{paths[name]}</svg>;
}

const endpointKey = (host, port) => `${String(host || "").trim().toLowerCase()}:${Number(port) || 0}`;

function formatRecentTime(value) {
  const timestamp = Number(value);
  if (!timestamp) return "最近连接";
  const elapsed = Date.now() - timestamp;
  if (elapsed < 60_000) return "刚刚连接";
  if (elapsed < 3_600_000) return `${Math.floor(elapsed / 60_000)} 分钟前`;
  if (elapsed < 86_400_000) return `${Math.floor(elapsed / 3_600_000)} 小时前`;
  return `${Math.floor(elapsed / 86_400_000)} 天前`;
}

function DeviceCard({ device, busy, onConnect, onRemove }) {
  return (
    <article className={`device-tile ${device.online ? "is-online" : "is-recent"}`}>
      <div className="device-tile-top">
        <span className="device-glyph"><Icon name="monitor" /></span>
        <span className={`availability ${device.online ? "online" : "recent"}`}>
          <i />{device.online ? "局域网在线" : "最近连接"}
        </span>
        {onRemove && (
          <button className="delete-device" title="从最近连接中移除" onClick={onRemove}>
            <Icon name="trash" />
          </button>
        )}
      </div>
      <div className="device-copy">
        <h3>{device.name || "Windows 设备"}</h3>
        <p>{device.host}:{device.port}</p>
        <small>{device.online ? "可直接建立局域网连接" : formatRecentTime(device.lastConnectedAtMs)}</small>
      </div>
      <button className="device-connect" disabled={busy} onClick={onConnect}>
        {busy ? "连接中" : "连接"}<Icon name="arrow" />
      </button>
    </article>
  );
}

function ConnectedSession({ state, wallpaperUrl, onDisconnect }) {
  const screenWidth = state.deviceInfo?.screenWidth || 0;
  const screenHeight = state.deviceInfo?.screenHeight || 0;
  const previewStyle = wallpaperUrl ? { backgroundImage: `url(${wallpaperUrl})` } : {};
  return (
    <section className="connected-view view-enter">
      <header className="session-heading">
        <div>
          <span className="session-badge"><i />已连接</span>
          <h2>{state.deviceInfo?.computerName || "远程设备"}</h2>
          <p>{state.host}:{state.port} · 局域网直连</p>
        </div>
        <div className="session-ready"><small>控制通道</small><strong>READY</strong></div>
      </header>
      <div className="session-layout">
        <article className="desktop-preview-card">
          <button className="wallpaper-preview" style={previewStyle} onClick={() => sendCommand("enterDesktop")}>
            {!wallpaperUrl && <span className="wallpaper-placeholder"><Icon name="monitor" />等待设备桌面信息</span>}
            <span className="wallpaper-shade" />
            <span className="enter-overlay">进入桌面 <Icon name="arrow" /></span>
          </button>
          <footer>
            <div><i /><span><strong>设备桌面入口</strong><small>点击后打开实时远程控制窗口</small></span></div>
            <button className="primary-button" onClick={() => sendCommand("enterDesktop")}>进入桌面 <Icon name="arrow" /></button>
          </footer>
        </article>
        <aside className="session-details">
          <span className="eyebrow">SESSION / READY</span>
          <h3>会话详情</h3>
          <dl>
            <div><dt>连接方式</dt><dd>局域网直连</dd></div>
            <div><dt>远程分辨率</dt><dd>{screenWidth > 0 ? `${screenWidth} × ${screenHeight}` : "获取中"}</dd></div>
            <div><dt>控制权限</dt><dd>键盘与鼠标</dd></div>
            <div><dt>视频状态</dt><dd>进入桌面后启动</dd></div>
          </dl>
          <button className="outline-button danger" onClick={onDisconnect}>断开连接</button>
        </aside>
      </div>
    </section>
  );
}

function DevicesPage({ state, devices, busy, connectDevice, removeDevice }) {
  return (
    <section className="content-view view-enter">
      <header className="page-heading">
        <div><span className="eyebrow">LOCAL DEVICES / 01</span><h1>我的设备</h1><p>发现同一网络中的电脑，也可以重新连接曾经使用过的设备。</p></div>
        <button className="refresh-button" onClick={() => sendCommand("refreshLanDevices")}><Icon name="refresh" />刷新设备</button>
      </header>
      {devices.length > 0 ? (
        <div className="device-grid">
          {devices.map((device) => (
            <DeviceCard
              key={device.key}
              device={device}
              busy={busy}
              onConnect={() => connectDevice(device)}
              onRemove={device.recentId ? () => removeDevice(device.recentId) : null}
            />
          ))}
        </div>
      ) : (
        <div className="empty-state">
          <div className="discovery-orbit"><span /><span /><span /><b>W</b></div>
          <strong>还没有发现设备</strong>
          <p>正在搜索局域网设备。如果校园网阻止广播，可以前往“远程协助”通过 IP 地址连接。</p>
          <button className="outline-button" onClick={() => sendCommand("refreshLanDevices")}><Icon name="refresh" />重新扫描</button>
        </div>
      )}
    </section>
  );
}

function AssistPage({ state, numericPort, setRole, connectManual, busy }) {
  const controlledActive = state.signalingState === "Listening" || state.sessionOpen;
  const remoteAccessEnabled = state.applicationSettings?.remoteAccessEnabled !== false;
  return (
    <section className="content-view assist-view view-enter">
      <header className="page-heading"><div><span className="eyebrow">REMOTE ASSISTANCE / 02</span><h1>远程协助</h1><p>在同一网络中，让两台设备自然相连。</p></div></header>
      <div className="assist-layout">
        <article className="assist-primary-panel">
          <div className="panel-title"><span>01</span><div><h2>选择本机角色</h2><p>连接前确认这台电脑由谁操作。</p></div></div>
          <div className="role-picker">
            <button className={state.role === "controller" ? "active" : ""} onClick={() => setRole("controller")}><Icon name="devices" /><span><strong>控制其他设备</strong><small>查看并操作另一台电脑</small></span></button>
            <button className={state.role === "controlled" ? "active" : ""} onClick={() => setRole("controlled")}><Icon name="assist" /><span><strong>允许远程控制</strong><small>等待另一台电脑连接本机</small></span></button>
          </div>

          {state.role === "controller" ? (
            <div className="address-form">
              <div className="form-copy"><span className="eyebrow">DIRECT ADDRESS</span><h3>通过地址连接</h3><p>适用于校园网或广播受限网络。</p></div>
              <label><span>IP 地址</span><input value={state.host} onChange={(event) => state.setHost(event.target.value)} placeholder="192.168.1.20" /></label>
              <label className="port-field"><span>端口</span><input value={state.port} onChange={(event) => state.setPort(event.target.value)} /></label>
              <button className="primary-button" disabled={busy} onClick={connectManual}>{busy ? "正在连接" : "连接设备"}<Icon name="arrow" /></button>
            </div>
          ) : (
            <div className="listen-panel">
              <div className={`listen-orbit ${controlledActive ? "active" : ""}`}><span /><span /><b>{controlledActive ? "ON" : "W"}</b></div>
              <div className="listen-copy"><span className="eyebrow">THIS DEVICE</span><h3>{state.sessionOpen ? "控制端已连接" : controlledActive ? "正在等待连接" : "准备接受连接"}</h3><p>{state.sessionOpen ? "远程控制会话正在进行，本机仍可随时结束。" : "保持窗口可见，启动后本机会响应局域网发现。"}</p></div>
              <label><span>监听端口</span><input value={state.port} onChange={(event) => state.setPort(event.target.value)} /></label>
              {!controlledActive ? (
                <button className="primary-button" disabled={!remoteAccessEnabled} onClick={() => sendCommand("startSignalingServer", { port: numericPort })}>{remoteAccessEnabled ? "开始监听" : "远程控制已关闭"}</button>
              ) : (
                <button className="outline-button danger" onClick={() => sendCommand("disconnectSession")}>{state.sessionOpen ? "结束控制" : "停止监听"}</button>
              )}
            </div>
          )}
        </article>
        <aside className="assist-status-panel">
          <span className="eyebrow">CONNECTION STATUS</span><h3>连接状态</h3>
          <div className="status-row"><span>信令</span><strong>{state.signalingState}</strong></div>
          <div className="status-row"><span>WebRTC</span><strong>{state.webrtcState}</strong></div>
          <div className="status-row"><span>控制通道</span><strong>{state.sessionOpen ? "Ready" : "—"}</strong></div>
          <div className="status-note"><Icon name="signal" /><p>{state.role === "controller" ? "优先使用自动发现；无法发现时使用 IP 地址连接。" : "被控端会保持可见，并提供明确的停止入口。"}</p></div>
        </aside>
      </div>
    </section>
  );
}

function SettingsPage({ state }) {
  const settings = state.applicationSettings;
  if (!settings) {
    return <section className="content-view settings-view view-enter"><div className="empty-state"><strong>正在读取设置</strong></div></section>;
  }

  const update = (patch) => sendCommand("updateApplicationSettings", { ...settings, ...patch });
  const changeRemoteAccess = () => {
    const enabled = !settings.remoteAccessEnabled;
    if (!enabled && state.sessionOpen && !window.confirm("关闭远程控制将立即结束当前会话，是否继续？")) return;
    update({ remoteAccessEnabled: enabled });
  };
  const changeApprovalMode = (approvalMode) => {
    if (approvalMode === "autoAccept" && settings.approvalMode !== "autoAccept"
      && !window.confirm("自动接受会允许能够访问监听端口的设备直接建立控制。仅建议在可信网络中使用，是否继续？")) return;
    update({ approvalMode });
  };
  const changeDefaultPort = (value) => {
    const defaultListenPort = Number.parseInt(value, 10);
    if (!defaultListenPort || defaultListenPort > 65535) return;
    if (state.signalingState !== "Listening" && !state.sessionOpen) {
      state.setPort(String(defaultListenPort));
    }
    update({ defaultListenPort });
  };

  return (
    <section className="content-view settings-view view-enter">
      <header className="page-heading"><div><span className="eyebrow">CONNECTION &amp; SAFETY / 03</span><h1>设置</h1><p>决定这台电脑如何接收连接，以及下次启动时使用的默认身份。</p></div></header>
      <div className="settings-layout">
        <article className="settings-card settings-hero">
          <div className="settings-icon"><Icon name="shield" /></div>
          <div><span className="eyebrow">REMOTE ACCESS</span><h2>允许远程控制</h2><p>关闭后将停止监听和局域网响应，其他设备无法再请求控制本机。</p></div>
          <button className={`switch-control ${settings.remoteAccessEnabled ? "is-on" : ""}`} role="switch" aria-checked={settings.remoteAccessEnabled} onClick={changeRemoteAccess}><span /></button>
        </article>

        <article className={`settings-card ${settings.remoteAccessEnabled ? "" : "is-disabled"}`}>
          <div className="settings-section-title"><span>01</span><div><h2>连接审批</h2><p>收到新的控制请求时，选择本机采取的动作。</p></div></div>
          <div className="approval-options">
            {[
              ["ask", "每次询问", "由本机用户确认后才开始协商", "推荐"],
              ["autoAccept", "自动接受", "适合自己管理的可信局域网", "风险"],
              ["deny", "始终拒绝", "保持配置但不允许建立新会话", "安静"],
            ].map(([value, title, detail, badge]) => (
              <button key={value} className={settings.approvalMode === value ? "active" : ""} disabled={!settings.remoteAccessEnabled} onClick={() => changeApprovalMode(value)}>
                <span className="radio-mark" /><span><strong>{title}</strong><small>{detail}</small></span><em>{badge}</em>
              </button>
            ))}
          </div>
          <label className="setting-line"><span><strong>确认倒计时</strong><small>超时后自动拒绝本次连接</small></span><select value={settings.approvalTimeoutSeconds} disabled={!settings.remoteAccessEnabled} onChange={(event) => update({ approvalTimeoutSeconds: Number(event.target.value) })}><option value="10">10 秒</option><option value="30">30 秒</option><option value="60">60 秒</option><option value="120">120 秒</option></select></label>
        </article>

        <article className="settings-card">
          <div className="settings-section-title"><span>02</span><div><h2>启动默认值</h2><p>角色在下次启动时生效，端口用于下一次开始监听。</p></div></div>
          <div className="settings-fields">
            <label><span>默认角色</span><select value={settings.defaultRole} onChange={(event) => update({ defaultRole: event.target.value })}><option value="controller">控制端</option><option value="controlled">被控端</option></select></label>
            <label><span>默认监听端口</span><input key={settings.defaultListenPort} defaultValue={settings.defaultListenPort} inputMode="numeric" onBlur={(event) => changeDefaultPort(event.target.value)} /></label>
          </div>
        </article>
      </div>
    </section>
  );
}

function AccessRequestModal({ request }) {
  const [seconds, setSeconds] = useState(() => Math.max(0, Math.ceil((request.expiresAtMs - Date.now()) / 1000)));
  const respond = (accepted) => sendCommand("respondIncomingAccessRequest", { requestId: request.requestId, accepted });
  useEffect(() => {
    const update = () => setSeconds(Math.max(0, Math.ceil((request.expiresAtMs - Date.now()) / 1000)));
    const timer = window.setInterval(update, 250);
    const onKeyDown = (event) => { if (event.key === "Escape") respond(false); };
    window.addEventListener("keydown", onKeyDown);
    return () => { window.clearInterval(timer); window.removeEventListener("keydown", onKeyDown); };
  }, [request.requestId, request.expiresAtMs]);
  return (
    <div className="approval-backdrop">
      <article className="approval-dialog">
        <div className="approval-rings"><span /><span /><Icon name="shield" /></div>
        <span className="eyebrow">INCOMING REQUEST</span>
        <h2>{request.deviceName}</h2>
        <p>请求查看并控制这台电脑</p>
        <dl><div><dt>来源地址</dt><dd>{request.sourceAddress}</dd></div><div><dt>自动拒绝</dt><dd>{seconds} 秒</dd></div><div><dt>控制权限</dt><dd>键盘与鼠标</dd></div></dl>
        <div className="approval-actions"><button className="outline-button danger" onClick={() => respond(false)}>拒绝</button><button className="primary-button" onClick={() => respond(true)}>允许本次连接</button></div>
        <small>按 Esc 可拒绝。允许后才会开始屏幕采集和 WebRTC 协商。</small>
      </article>
    </div>
  );
}

export function DashboardPage() {
  const state = useNativeState();
  const [activePage, setActivePage] = useState("devices");
  const numericPort = Number.parseInt(state.port, 10) || 39000;
  const remoteOnline = state.role === "controller" && state.sessionOpen;
  const awaitingApproval = state.webrtcState === "AwaitingApproval";
  const busy = state.signalingState === "Connecting" || ["Connecting", "AwaitingApproval", "Negotiating"].includes(state.webrtcState);
  const wallpaperUrl = state.deviceInfo?.wallpaperData
    ? `data:${state.deviceInfo.wallpaperMime || "image/jpeg"};base64,${state.deviceInfo.wallpaperData}`
    : "";

  useEffect(() => {
    if (state.applicationSettings && state.role === "controlled" && activePage === "devices") {
      setActivePage("assist");
    }
  }, [state.applicationSettings, state.role, activePage]);

  const mergedDevices = useMemo(() => {
    const onlineByEndpoint = new Map(state.lanDevices.map((device) => [endpointKey(device.address, device.port), device]));
    const devices = state.recentDevices.map((recent) => {
      const key = endpointKey(recent.host, recent.port);
      const online = onlineByEndpoint.get(key);
      onlineByEndpoint.delete(key);
      return {
        key: `recent-${recent.deviceId}`,
        recentId: recent.deviceId,
        discoveryId: online?.deviceId || "",
        name: online?.name || recent.name,
        host: online?.address || recent.host,
        port: online?.port || recent.port,
        lastConnectedAtMs: recent.lastConnectedAtMs,
        online: Boolean(online),
      };
    });
    onlineByEndpoint.forEach((device) => devices.push({
      key: `lan-${device.deviceId}`,
      discoveryId: device.deviceId,
      name: device.name,
      host: device.address,
      port: device.port,
      online: true,
    }));
    return devices.sort((left, right) => Number(right.online) - Number(left.online));
  }, [state.lanDevices, state.recentDevices]);

  const beginConnection = (device) => {
    state.setHost(device.host);
    state.setPort(String(device.port));
    if (device.discoveryId) sendCommand("connectLanDevice", { deviceId: device.discoveryId });
    else sendCommand("connectRecentDevice", { deviceId: device.recentId });
  };

  const setRole = (role) => {
    state.setRole(role);
    if (role === "controlled") setActivePage("assist");
  };

  const errors = [state.error, state.recentDeviceError, state.applicationSettingsError].filter(Boolean);

  return (
    <main className="dashboard-shell">
      <aside className="app-sidebar">
        <div className="app-brand"><span>W</span><div><strong>winRemote</strong><small>Control</small></div></div>
        <nav className="app-nav">
          <button className={activePage === "devices" ? "active" : ""} onClick={() => { setRole("controller"); setActivePage("devices"); sendCommand("requestRecentDevices"); }}><Icon name="devices" /><span>我的设备</span></button>
          <button className={activePage === "assist" ? "active" : ""} onClick={() => setActivePage("assist")}><Icon name="assist" /><span>远程协助</span></button>
        </nav>
        <div className="sidebar-recents">
          <span>最近连接</span>
          {state.recentDevices.slice(0, 3).map((device) => {
            const online = state.lanDevices.some((item) => endpointKey(item.address, item.port) === endpointKey(device.host, device.port));
            return <button key={device.deviceId} onClick={() => beginConnection({ ...device, recentId: device.deviceId, online })}><i className={online ? "online" : ""} /><span>{device.name || device.host}</span></button>;
          })}
          {state.recentDevices.length === 0 && <small>成功连接后会显示在这里</small>}
        </div>
        <button className={`settings-nav-button ${activePage === "settings" ? "active" : ""}`} onClick={() => { setActivePage("settings"); sendCommand("requestApplicationSettings"); }}><Icon name="settings" /><span>设置</span></button>
        <div className="sidebar-foot"><span className="privacy-dot" /><p>仅局域网连接<br /><small>连接记录保存在本机</small></p></div>
      </aside>

      <div className="dashboard-main">
        <header className="dashboard-topbar"><span>winRemoteControl</span><div className={`discovery-state ${state.lanDiscoveryError ? "limited" : ""}`}><i />{state.lanDiscoveryError ? "广播受限" : "局域网已就绪"}</div></header>
        {activePage === "settings" ? (
          <SettingsPage state={state} />
        ) : remoteOnline ? (
          <ConnectedSession state={state} wallpaperUrl={wallpaperUrl} onDisconnect={() => sendCommand("disconnectSession")} />
        ) : activePage === "devices" ? (
          <DevicesPage
            state={state}
            devices={mergedDevices}
            busy={busy}
            connectDevice={beginConnection}
            removeDevice={(deviceId) => sendCommand("removeRecentDevice", { deviceId })}
          />
        ) : activePage === "assist" ? (
          <AssistPage
            state={state}
            numericPort={numericPort}
            setRole={setRole}
            busy={busy}
            connectManual={() => sendCommand("connectSignaling", { host: state.host.trim(), port: numericPort })}
          />
        ) : null}
        {awaitingApproval && state.role === "controller" && <div className="approval-waiting"><span className="waiting-pulse" /><div><strong>等待对方确认</strong><small>被控端允许后才会建立远程会话</small></div><button onClick={() => sendCommand("disconnectSession")}>取消</button></div>}
        {state.incomingAccessRequest && <AccessRequestModal request={state.incomingAccessRequest} />}
        {errors.length > 0 && <div className="error-stack">{errors.map((error, index) => <p key={`${index}-${error}`}>{error}</p>)}</div>}
      </div>
    </main>
  );
}

export function DesktopPage() {
  const state = useNativeState();
  const previewSlotRef = useRef(null);
  const [elapsedSeconds, setElapsedSeconds] = useState(0);
  const sessionUnavailable = ["Interrupted", "Stopping", "Disconnected", "Failed"].includes(state.webrtcState);
  const qualityPresets = {
    auto: { fps: 30, width: 1280, height: 720, bitrateKbps: 3000 },
  };

  const formatElapsed = (seconds) => {
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const remaining = seconds % 60;
    const pad = (value) => String(value).padStart(2, "0");
    return hours > 0 ? `${pad(hours)}:${pad(minutes)}:${pad(remaining)}` : `${pad(minutes)}:${pad(remaining)}`;
  };

  const networkClass = useMemo(() => {
    if (state.networkStats.quality === "excellent") return "network-dot is-excellent";
    if (state.networkStats.quality === "good") return "network-dot is-good";
    if (state.networkStats.quality === "poor") return "network-dot is-bad";
    const value = state.webrtcState.toLowerCase();
    if (value.includes("failed") || value.includes("disconnect")) return "network-dot is-bad";
    if (value.includes("checking") || value.includes("connecting")) return "network-dot is-warn";
    return state.fps > 0 ? "network-dot is-good" : "network-dot";
  }, [state.fps, state.networkStats.quality, state.webrtcState]);

  const networkLabel = state.networkStats.rttMs >= 0 ? `${state.networkStats.rttMs} ms` : "";
  const networkTitle = state.networkStats.rttMs >= 0
    ? `RTT ${state.networkStats.rttMs}ms / 抖动 ${state.networkStats.jitterMs}ms / 丢包 ${(state.networkStats.packetLossRate * 100).toFixed(1)}%`
    : "网络统计采集中";

  useEffect(() => {
    const element = previewSlotRef.current;
    if (!element) return undefined;
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
    const timerId = window.setInterval(() => setElapsedSeconds((value) => value + 1), 1000);
    return () => window.clearInterval(timerId);
  }, []);

  useEffect(() => {
    sendCommand("setStreamConfig", qualityPresets.auto);
  }, []);

  return (
    <main className="desktop-shell">
      <header className="desktop-titlebar" onPointerDown={(event) => {
        if (event.button === 0 && event.target.closest("[data-window-control]") === null) sendCommand("beginDesktopWindowDrag");
      }}>
        <div className="desktop-title-left">
          <span className="desktop-mark">W</span>
          <strong>{state.deviceInfo?.computerName || "远程桌面"}</strong>
          <span className="network-status"><i className={networkClass} title={networkTitle} />{networkLabel && <b>{networkLabel}</b>}<span>{formatElapsed(elapsedSeconds)}</span></span>
          <span className="desktop-stat">{state.frame ? `${state.frame.width} × ${state.frame.height}` : "等待画面"}</span>
          <span className="desktop-stat">{state.fps} FPS</span>
        </div>
        <DesktopWindowControls />
      </header>
      <section className="desktop-stage">
        <div ref={previewSlotRef} className="native-preview-slot desktop-slot" />
        {sessionUnavailable && (
          <div className="desktop-disconnected">
            <div className="disconnect-orbit"><span /><span /><b>!</b></div>
            <strong>{state.webrtcState === "Interrupted" ? "连接暂时中断" : "远程连接已断开"}</strong>
            <p>{state.webrtcState === "Interrupted" ? "正在等待网络恢复，恢复前不会继续发送输入。" : "本次远程控制已经结束，请返回主界面重新连接。"}</p>
            {state.webrtcState !== "Interrupted" && <button className="primary-button" onClick={() => sendCommand("closeDesktop")}>返回主界面</button>}
          </div>
        )}
      </section>
      {state.error && <p className="desktop-error">{state.error}</p>}
    </main>
  );
}
