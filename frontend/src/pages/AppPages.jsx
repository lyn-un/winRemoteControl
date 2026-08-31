import React, { useEffect, useMemo, useRef, useState } from "react";
import { sendCommand, sendPreviewRect } from "../bridge/nativeBridge";
import DesktopWindowControls from "../components/DesktopWindowControls";
import { useNativeState } from "../state/useNativeState";
import { useTheme } from "../theme/useTheme";

function Icon({ name }) {
  const paths = {
    devices: <><rect x="3" y="4" width="18" height="13" rx="2" /><path d="M8 21h8M12 17v4" /></>,
    assist: <><circle cx="6" cy="12" r="3" /><circle cx="18" cy="6" r="3" /><circle cx="18" cy="18" r="3" /><path d="m9 11 6-4M9 13l6 4" /></>,
    refresh: <><path d="M20 6v5h-5M4 18v-5h5" /><path d="M6.1 9a7 7 0 0 1 11.5-2.6L20 9M4 15l2.4 2.6A7 7 0 0 0 17.9 15" /></>,
    monitor: <><rect x="3" y="4" width="18" height="14" rx="2" /><path d="M9 22h6M12 18v4" /></>,
    trash: <><path d="M4 7h16M9 7V4h6v3M7 7l1 14h8l1-14M10 11v6M14 11v6" /></>,
    arrow: <><path d="M5 12h14M14 7l5 5-5 5" /></>,
    signal: <><path d="M5 15a10 10 0 0 1 14 0M8 18a6 6 0 0 1 8 0" /><circle cx="12" cy="21" r="1" /></>,
    appearance: <><path d="M12 3a9 9 0 1 0 0 18h1.3a1.7 1.7 0 0 0 .3-3.37 1.35 1.35 0 0 1 .25-2.67H17A4 4 0 0 0 21 11c0-4.42-4.03-8-9-8Z" /><circle cx="7.5" cy="11" r="1" /><circle cx="10" cy="7" r="1" /><circle cx="15" cy="7.5" r="1" /></>,
    settings: <><circle cx="12" cy="12" r="3" /><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.88l.06.06-2.83 2.83-.06-.06A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 .6 1.7 1.7 0 0 0-.4 1.1V21h-4v-.09A1.7 1.7 0 0 0 8.6 19.4a1.7 1.7 0 0 0-1.88.34l-.06.06-2.83-2.83.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-.6-1 1.7 1.7 0 0 0-1.1-.4H3v-4h.09A1.7 1.7 0 0 0 4.6 8.6a1.7 1.7 0 0 0-.34-1.88l-.06-.06 2.83-2.83.06.06A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-.6 1.7 1.7 0 0 0 .4-1.1V3h4v.09A1.7 1.7 0 0 0 15.4 4.6a1.7 1.7 0 0 0 1.88-.34l.06-.06 2.83 2.83-.06.06A1.7 1.7 0 0 0 19.4 9c.14.37.35.7.6 1 .3.28.68.42 1.1.4h.09v4h-.09A1.7 1.7 0 0 0 19.4 15Z" /></>,
    shield: <><path d="M12 3 5 6v5c0 4.6 2.8 8.2 7 10 4.2-1.8 7-5.4 7-10V6l-7-3Z" /><path d="m9 12 2 2 4-4" /></>,
		terminal: <><rect x="3" y="4" width="18" height="16" rx="2" /><path d="m7 9 3 3-3 3M13 15h4" /></>,
		fileTransfer: <><path d="M3 7h7l2 2h9v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z" /><path d="M8 14h8M13 11l3 3-3 3" /></>,
    chevron: <path d="m8 10 4 4 4-4" />,
  };
  return <svg className="ui-icon" viewBox="0 0 24 24" aria-hidden="true">{paths[name]}</svg>;
}

function MainWindowControls() {
  return (
    <div className="main-window-controls" data-window-control>
      <button aria-label="最小化" title="最小化" onClick={() => sendCommand("minimizeMainWindow")}>
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 16h12" /></svg>
      </button>
      <button className="is-close" aria-label="关闭" title="关闭" onClick={() => sendCommand("closeMainWindow")}>
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m7 7 10 10M17 7 7 17" /></svg>
      </button>
    </div>
  );
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

function ConnectedSession({ state, wallpaperUrl, onDisconnect, onOpenTerminal, onOpenFileTransfer }) {
  const screenWidth = state.deviceInfo?.screenWidth || 0;
  const screenHeight = state.deviceInfo?.screenHeight || 0;
  const previewStyle = wallpaperUrl ? { backgroundImage: `url(${wallpaperUrl})` } : {};
  const terminalState = state.terminalState?.state || "Closed";
  const terminalBusy = ["AwaitingApproval", "Opening", "Closing"].includes(terminalState);
  const terminalRunning = ["Running", "Paused"].includes(terminalState);
  const terminalSupported = state.terminalFrontendSupport.supported && state.terminalState?.available;
  const terminalReason = !state.terminalFrontendSupport.supported
    ? state.terminalFrontendSupport.reason || "未安装 Windows Terminal"
    : !state.terminalState?.available
      ? "对方不支持远程终端，或终端通道尚未就绪"
      : state.terminalState.status || "打开远程 PowerShell";
  const terminalLabel = terminalBusy ? "终端连接中" : terminalRunning ? "聚焦终端" : "终端";
	const transferState = state.fileTransferState?.state || "Closed";
	const transferBusy = ["Opening", "WaitingChannels", "Reconnecting", "Closing"].includes(transferState);
	const transferOpen = transferState === "Ready";
	const transferSupported = Boolean(state.fileTransferState?.available);
	const transferReason = transferSupported
		? state.fileTransferState.status || "在独立窗口中浏览并复制文件"
		: state.fileTransferState?.status || "当前可信设备未授予文件传输权限";
	const transferLabel = transferBusy ? "正在连接" : transferOpen ? "聚焦传输" : "文件传输";
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
            <div><i /><span><strong>设备桌面入口</strong></span></div>
            <span className="desktop-entry-actions">
			  <button className="terminal-action-button" disabled={!transferSupported || transferBusy} title={transferReason} onClick={onOpenFileTransfer}><Icon name="fileTransfer" />{transferLabel}{state.fileTransferState?.activeTasks > 0 ? ` · ${state.fileTransferState.activeTasks}` : ""}</button>
              <button className="terminal-action-button" disabled={!terminalSupported || terminalBusy} title={terminalReason} onClick={onOpenTerminal}><Icon name="terminal" />{terminalLabel}</button>
            </span>
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

function DeviceDetails({ device, busy, onConnect }) {
  const status = device.online ? "局域网在线" : "最近连接";
  return (
    <section className="connected-view device-detail-view view-enter">
      <header className="session-heading">
        <div>
          <span className={`session-badge ${device.online ? "" : "is-recent"}`}><i />{status}</span>
          <h2>{device.name || "Windows 设备"}</h2>
          <p>{device.host}:{device.port} · {status}</p>
        </div>
        <div className="session-ready"><small>设备状态</small><strong>{device.online ? "ONLINE" : "RECENT"}</strong></div>
      </header>
      <div className="session-layout">
        <article className="desktop-preview-card">
          <div className="wallpaper-preview device-placeholder-preview">
            <span className="device-mist-rings" />
            <span className="wallpaper-placeholder"><Icon name="monitor" /><strong>设备桌面</strong><small>建立连接后获取桌面入口</small></span>
          </div>
          <footer>
            <div><i className={device.online ? "" : "is-recent"} /><span><strong>{device.online ? "设备可以连接" : "曾经连接过此设备"}</strong><small>{device.online ? "局域网发现服务刚刚确认了设备状态" : formatRecentTime(device.lastConnectedAtMs)}</small></span></div>
            <button className="primary-button" disabled={busy} onClick={onConnect}>{busy ? "正在连接" : "连接设备"}<Icon name="arrow" /></button>
          </footer>
        </article>
        <aside className="session-details">
          <span className="eyebrow">DEVICE / {device.online ? "ONLINE" : "RECENT"}</span>
          <h3>设备详情</h3>
          <dl>
            <div><dt>设备名称</dt><dd>{device.name || "Windows 设备"}</dd></div>
            <div><dt>IP 地址</dt><dd>{device.host}</dd></div>
            <div><dt>信令端口</dt><dd>{device.port}</dd></div>
            <div><dt>设备来源</dt><dd>{device.online ? "局域网发现" : "本机记录"}</dd></div>
          </dl>
          <p className="device-detail-note">连接成功后，终端与远程桌面入口会显示在左侧。</p>
        </aside>
      </div>
    </section>
  );
}

function DevicesPage({ selectedDevice, busy, connectDevice }) {
  if (selectedDevice) {
    return <DeviceDetails device={selectedDevice} busy={busy} onConnect={() => connectDevice(selectedDevice)} />;
  }
  return (
    <section className="content-view view-enter">
      <header className="page-heading">
        <div><span className="eyebrow">LOCAL DEVICES / 01</span><h1>我的设备</h1><p>发现同一网络中的电脑，也可以重新连接曾经使用过的设备。</p></div>
        <button className="refresh-button" onClick={() => sendCommand("refreshLanDevices")}><Icon name="refresh" />刷新设备</button>
      </header>
      <div className="empty-state">
        <div className="discovery-orbit"><span /><span /><span /><b>W</b></div>
        <strong>从左侧选择一台设备</strong>
        <p>设备列表会合并局域网发现和最近连接记录。未发现设备时，也可以前往“远程协助”通过 IP 地址连接。</p>
        <button className="outline-button" onClick={() => sendCommand("refreshLanDevices")}><Icon name="refresh" />重新扫描</button>
      </div>
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
            <button className={state.role === "controller" ? "active" : ""} disabled={busy} onClick={() => setRole("controller")}><Icon name="devices" /><span><strong>控制其他设备</strong><small>查看并操作另一台电脑</small></span></button>
            <button className={state.role === "controlled" ? "active" : ""} disabled={busy} onClick={() => setRole("controlled")}><Icon name="assist" /><span><strong>允许远程控制</strong><small>等待另一台电脑连接本机</small></span></button>
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
  const theme = useTheme();
  const themeCardRefs = useRef([]);
  const [activeSection, setActiveSection] = useState("security");
  const [confirmation, setConfirmation] = useState("");
  useEffect(() => {
    if (!confirmation) return undefined;
    const onKeyDown = (event) => {
      if (event.key === "Escape") setConfirmation("");
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [confirmation]);
  if (!settings) {
    return <section className="content-view settings-view view-enter"><div className="empty-state"><strong>正在读取设置</strong></div></section>;
  }

  const update = (patch) => sendCommand("updateApplicationSettings", { ...settings, ...patch });
  const changeRemoteAccess = () => {
    const enabled = !settings.remoteAccessEnabled;
    if (!enabled && state.sessionOpen) {
      setConfirmation("disableRemoteAccess");
      return;
    }
    update({ remoteAccessEnabled: enabled });
  };
  const changeApprovalMode = (approvalMode) => {
    if (approvalMode === "autoAccept" && settings.approvalMode !== "autoAccept") {
      setConfirmation("autoAccept");
      return;
    }
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

  const sections = [
    { id: "security", icon: "shield", title: "连接与安全", detail: "远程接入和审批策略" },
    { id: "trusted", icon: "monitor", title: "可信设备", detail: "配对身份与权限上限" },
    { id: "network", icon: "signal", title: "网络与传输", detail: "监听端口与连接方式" },
    { id: "appearance", icon: "appearance", title: "外观设置", detail: "主题与界面显示" },
  ];
  const active = sections.find((section) => section.id === activeSection) || sections[0];
  const confirmationCopy = confirmation === "disableRemoteAccess"
    ? {
        eyebrow: "ACTIVE SESSION",
        title: "关闭远程控制？",
        detail: "当前远程会话将立即结束，同时停止监听和局域网响应。",
        note: "远端设备会断开，未保存的远端操作可能丢失。",
        confirmLabel: "结束会话并关闭",
        tone: "danger",
      }
    : {
        eyebrow: "TRUSTED NETWORK ONLY",
        title: "启用自动接受？",
        detail: "能够访问监听端口的设备将不再需要本机确认，即可进入远程控制协商。",
        note: "仅建议在由你管理、且成员可信的网络中启用。",
        confirmLabel: "确认启用",
        tone: "warning",
      };
  const confirmSettingsChange = () => {
    if (confirmation === "disableRemoteAccess")
      update({ remoteAccessEnabled: false });
    else if (confirmation === "autoAccept")
      update({ approvalMode: "autoAccept" });
    setConfirmation("");
  };

  const handleThemeKeyDown = (event, index) => {
    const lastIndex = theme.themes.length - 1;
    let nextIndex = index;
    if (["ArrowRight", "ArrowDown"].includes(event.key)) nextIndex = index === lastIndex ? 0 : index + 1;
    else if (["ArrowLeft", "ArrowUp"].includes(event.key)) nextIndex = index === 0 ? lastIndex : index - 1;
    else if (event.key === "Home") nextIndex = 0;
    else if (event.key === "End") nextIndex = lastIndex;
    else return;
    event.preventDefault();
    themeCardRefs.current[nextIndex]?.focus();
    theme.setTheme(theme.themes[nextIndex].id);
  };

  const renderSection = () => {
    if (activeSection === "security") {
      return (
        <div className="settings-content-stack">
          <article className="settings-card settings-hero settings-hero-contained">
            <div className="settings-icon"><Icon name="shield" /></div>
            <div><span className="eyebrow">REMOTE ACCESS</span><h2>允许远程控制</h2><p>关闭后将停止监听和局域网响应，其他设备无法再请求控制本机。</p></div>
            <button className={`switch-control ${settings.remoteAccessEnabled ? "is-on" : ""}`} role="switch" aria-checked={settings.remoteAccessEnabled} onClick={changeRemoteAccess}><span /></button>
          </article>
          <article className={`settings-card settings-content-card ${settings.remoteAccessEnabled ? "" : "is-disabled"}`}>
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
        </div>
      );
    }

    if (activeSection === "network") {
      return (
        <article className="settings-card settings-content-card">
          <div className="settings-section-title"><span>02</span><div><h2>本机监听</h2><p>配置被控端下一次开始监听时使用的 TCP 信令端口。</p></div></div>
          <div className="settings-fields settings-fields-wide">
            <label><span>默认监听端口</span><input key={settings.defaultListenPort} defaultValue={settings.defaultListenPort} inputMode="numeric" onBlur={(event) => changeDefaultPort(event.target.value)} /><small>有效范围为 1～65535；已经开始的监听不会自动重启。</small></label>
          </div>
        </article>
      );
    }
    if (activeSection === "appearance") {
      return (
        <article className="settings-card settings-content-card appearance-settings-card">
          <div className="settings-section-title"><span>04</span><div><h2>界面主题</h2><p>选择适合当前环境的界面明暗与色彩层次。</p></div></div>
          <div className="theme-card-grid" role="radiogroup" aria-label="界面主题">
            {theme.themes.map((item, index) => {
              const selected = theme.themeId === item.id;
              const previewStyle = {
                "--preview-canvas": item.preview.canvas,
                "--preview-sidebar": item.preview.sidebar,
                "--preview-panel": item.preview.panel,
                "--preview-accent": item.preview.accent,
              };
              return (
                <button
                  key={item.id}
                  ref={(element) => { themeCardRefs.current[index] = element; }}
                  className={`theme-card ${selected ? "is-selected" : ""}`}
                  role="radio"
                  aria-checked={selected}
                  tabIndex={selected ? 0 : -1}
                  onClick={() => theme.setTheme(item.id)}
                  onKeyDown={(event) => handleThemeKeyDown(event, index)}
                >
                  <span className="theme-preview" style={previewStyle} aria-hidden="true">
                    <span className="theme-preview-sidebar"><i /><i /><i /></span>
                    <span className="theme-preview-content"><i /><span><b /><b /></span></span>
                  </span>
                  <span className="theme-card-copy"><strong>{item.name}</strong><em>{item.englishName}</em><small>{item.description}</small></span>
                  <span className="theme-card-status">{selected ? "当前使用" : "选择主题"}</span>
                </button>
              );
            })}
          </div>
        </article>
      );
    }
    if (activeSection === "trusted") {
      const permissionLabels = { viewScreen: "查看画面", inputControl: "键鼠控制", clipboard: "剪贴板", terminal: "终端", fileTransfer: "文件传输" };
      const updateTrusted = (device, patch) => sendCommand("updateTrustedDevice", {
        deviceId: device.deviceId,
        alias: patch.alias ?? device.name,
        permissions: patch.permissions ?? device.permissions,
      });
      return (
        <div className="settings-content-stack trusted-device-list">
		  {state.securityMigrationNotice && <div className="security-migration-notice"><Icon name="shield" /><span>{state.securityMigrationNotice}</span></div>}
          {state.trustedDevices.length === 0 ? (
			<article className="settings-card settings-content-card"><div className="empty-state"><strong>暂无可信设备</strong><small>首次连接在两台电脑上确认六位配对数字后，设备会显示在这里。</small></div></article>
          ) : state.trustedDevices.map((device) => (
            <article key={device.deviceId} className={`settings-card settings-content-card trusted-device-item ${device.revoked ? "is-revoked" : ""}`}>
              <div className="settings-section-title"><span>●</span><div><h2>{device.name || "Windows 设备"}</h2><p>指纹 {device.fingerprint} · {device.revoked ? "已撤销" : "已验证"}</p></div></div>
              <label className="trusted-alias"><span>设备别名</span><input defaultValue={device.name || ""} maxLength={128} disabled={device.revoked} onBlur={(event) => updateTrusted(device, { alias: event.target.value })} /></label>
              <div className="trusted-permissions">
                {Object.entries(permissionLabels).map(([permission, label]) => {
                  const checked = device.permissions.includes(permission);
                  return <label key={permission}><input type="checkbox" checked={permission === "viewScreen" || checked} disabled={device.revoked || permission === "viewScreen"} onChange={() => updateTrusted(device, { permissions: checked ? device.permissions.filter((item) => item !== permission) : [...device.permissions, permission] })} /><span>{label}</span></label>;
                })}
              </div>
              <footer className="trusted-device-actions">
                <button className="outline-button" onClick={() => sendCommand("requestRePairDevice", { deviceId: device.deviceId })}>重新配对</button>
                {!device.revoked && <button className="danger-button" onClick={() => sendCommand("revokeTrustedDevice", { deviceId: device.deviceId })}>撤销信任</button>}
              </footer>
            </article>
          ))}
          {state.trustedDeviceError && <p className="form-error">{state.trustedDeviceError}</p>}
        </div>
      );
    }
    return null;
  };

  return (
    <section className="content-view settings-view view-enter">
      <div className="settings-workspace">
        <header className="page-heading settings-page-heading"><div><span className="eyebrow">PREFERENCES / 04</span><h1>设置</h1></div></header>
        <header className="settings-pane-heading"><div><span className="eyebrow">AVAILABLE NOW</span><h2>{active.title}</h2><p>{active.detail}</p></div><span className="settings-pane-index">{String(sections.indexOf(active) + 1).padStart(2, "0")}</span></header>
        <nav className="settings-index" aria-label="设置分类">
          <div className="settings-index-heading"><span>设置目录</span><small>SETTINGS INDEX</small></div>
          {sections.map((section, index) => (
            <button key={section.id} className={activeSection === section.id ? "active" : ""} onClick={() => { setActiveSection(section.id); if (section.id === "trusted") sendCommand("requestTrustedDevices"); }}>
              <span className="settings-index-number">{String(index + 1).padStart(2, "0")}</span>
              <span className="settings-index-icon"><Icon name={section.icon} /></span>
              <span><strong>{section.title}</strong><small>{section.detail}</small></span>
            </button>
          ))}
        </nav>
        <div className="settings-pane">
          {renderSection()}
        </div>
      </div>
      {confirmation && (
        <div className="settings-confirm-backdrop" onMouseDown={() => setConfirmation("")}>
          <article className={`settings-confirm-dialog ${confirmationCopy.tone}`} role="dialog" aria-modal="true" aria-labelledby="settings-confirm-title" onMouseDown={(event) => event.stopPropagation()}>
            <header>
              <span className="settings-confirm-mark"><Icon name="shield" /></span>
              <div><span className="eyebrow">{confirmationCopy.eyebrow}</span><h2 id="settings-confirm-title">{confirmationCopy.title}</h2></div>
            </header>
            <p>{confirmationCopy.detail}</p>
            <div className="settings-confirm-note"><span>!</span><small>{confirmationCopy.note}</small></div>
            <footer><button className="outline-button" onClick={() => setConfirmation("")}>取消</button><button className="primary-button" onClick={confirmSettingsChange}>{confirmationCopy.confirmLabel}</button></footer>
          </article>
        </div>
      )}
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

function PairingRequestModal({ request }) {
  const [seconds, setSeconds] = useState(() => Math.max(0, Math.ceil((request.expiresAtMs - Date.now()) / 1000)));
  const [permissions, setPermissions] = useState(() => new Set(request.permissions));
	const [step, setStep] = useState("verify");
  const permissionLabels = { viewScreen: "查看画面", inputControl: "键盘与鼠标", clipboard: "剪贴板", terminal: "远程终端", fileTransfer: "文件传输" };
  const respond = (accepted) => sendCommand("respondPairingRequest", { requestId: request.requestId, accepted, permissions: [...permissions] });
  const toggle = (permission) => setPermissions((current) => {
    const next = new Set(current);
    if (permission !== "viewScreen") next.has(permission) ? next.delete(permission) : next.add(permission);
    return next;
  });
  useEffect(() => {
    const timer = window.setInterval(() => setSeconds(Math.max(0, Math.ceil((request.expiresAtMs - Date.now()) / 1000))), 250);
    return () => window.clearInterval(timer);
  }, [request.expiresAtMs]);
	useEffect(() => {
		setStep("verify");
		setPermissions(new Set(request.permissions));
	}, [request.requestId]);
	const confirmCode = () => {
		if (request.localRole === "controlled") {
			setStep("permissions");
			return;
		}
		respond(true);
		setStep("waiting");
	};
  return <div className="approval-backdrop"><article className="approval-dialog pairing-dialog">
    <div className="approval-rings"><span /><span /><Icon name="shield" /></div>
		<span className="eyebrow">DEVICE PAIRING</span>
		<h2>{step === "permissions" ? "设置设备权限" : "确认配对数字"}</h2>
		{step === "permissions"
			? <p>数字已确认一致。请选择这台设备以后最多可以使用的权限。</p>
			: <p>请确认两台电脑显示的六位数字完全一致。</p>}
		{step !== "permissions" && <div className="pairing-code" aria-label={`配对码 ${request.verificationCode}`}>{request.verificationCode}</div>}
    <dl><div><dt>设备</dt><dd>{request.deviceName}</dd></div><div><dt>自动拒绝</dt><dd>{seconds} 秒</dd></div></dl>
		{step === "permissions" && <div className="pairing-permissions">{request.permissions.map((permission) => <label key={permission}><input type="checkbox" checked={permissions.has(permission)} disabled={permission === "viewScreen"} onChange={() => toggle(permission)} />{permissionLabels[permission] || permission}</label>)}</div>}
		{step === "verify" && <div className="approval-actions"><button className="outline-button danger" onClick={() => respond(false)}>数字不一致</button><button className="primary-button" onClick={confirmCode}>数字一致</button></div>}
		{step === "permissions" && <div className="approval-actions"><button className="outline-button danger" onClick={() => respond(false)}>取消配对</button><button className="primary-button" onClick={() => respond(true)}>保存权限并配对</button></div>}
		{step === "waiting" && <div className="pairing-waiting">已确认，正在等待被控端确认…</div>}
		<details className="pairing-security-details">
			<summary>查看安全详情</summary>
			<div className="pairing-fingerprints">
				<div><strong>控制端证书</strong><code>{request.controllerFingerprint}</code></div>
				<div><strong>被控端证书</strong><code>{request.controlledFingerprint}</code></div>
			</div>
			<dl><div><dt>TLS</dt><dd>{request.tlsProtocol || "—"}</dd></div><div><dt>密码套件</dt><dd>{request.cipherSuite || "—"}</dd></div></dl>
		</details>
		<small>配对数字由本次 Schannel TLS 安全通道生成，不会通过网络发送。确认后，本机会固定对方的设备公钥。</small>
  </article></div>;
}

function TerminalRequestModal({ request }) {
	const [seconds, setSeconds] = useState(() => Math.max(0, Math.ceil((request.expiresAtMs - Date.now()) / 1000)));
	const respond = (accepted) => sendCommand("respondTerminalAccessRequest", { requestId: request.requestId, accepted });
	useEffect(() => {
		const timer = window.setInterval(() => setSeconds(Math.max(0, Math.ceil((request.expiresAtMs - Date.now()) / 1000))), 250);
		return () => window.clearInterval(timer);
	}, [request.expiresAtMs]);
	return <div className="approval-backdrop"><article className="approval-dialog terminal-approval">
		<div className="approval-rings"><span /><span /><Icon name="terminal" /></div>
		<span className="eyebrow">REMOTE TERMINAL</span><h2>允许打开 PowerShell？</h2>
		<p>{request.deviceName || "控制端"}请求在本机启动一个普通用户权限的 PowerShell。</p>
		<dl><div><dt>权限</dt><dd>当前登录用户</dd></div><div><dt>自动拒绝</dt><dd>{seconds} 秒</dd></div></dl>
		{request.sourceAddress && <small>来源地址：{request.sourceAddress}</small>}
		<div className="approval-actions"><button className="outline-button danger" onClick={() => respond(false)}>拒绝</button><button className="primary-button" onClick={() => respond(true)}>允许本次会话</button></div>
		<small>终端运行期间会在本机界面持续显示状态，可随时停止。</small>
	</article></div>;
}

export function DashboardPage() {
  const state = useNativeState();
  const [activePage, setActivePage] = useState("devices");
  const [devicesExpanded, setDevicesExpanded] = useState(true);
  const [selectedEndpoint, setSelectedEndpoint] = useState("");
  const [deviceContextMenu, setDeviceContextMenu] = useState(null);
  const numericPort = Number.parseInt(state.port, 10) || 39000;
  const remoteOnline = state.role === "controller" && state.sessionOpen;
  const awaitingApproval = state.webrtcState === "AwaitingApproval";
  const busy = state.signalingState === "Connecting" || ["Connecting", "AwaitingApproval", "Negotiating", "Stopping", "ShutdownTimedOut"].includes(state.webrtcState);
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
    const devices = state.recentDevices.filter((recent) => !recent.incoming).map((recent) => {
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
    return devices.sort((left, right) => {
      const onlineOrder = Number(right.online) - Number(left.online);
      if (onlineOrder !== 0) return onlineOrder;
      const recentOrder = Number(right.lastConnectedAtMs || 0) - Number(left.lastConnectedAtMs || 0);
      if (recentOrder !== 0) return recentOrder;
      return String(left.name || left.host).localeCompare(String(right.name || right.host), "zh-CN");
    });
  }, [state.lanDevices, state.recentDevices]);

  const selectedDevice = useMemo(() => mergedDevices.find((device) => endpointKey(device.host, device.port) === selectedEndpoint) || null, [mergedDevices, selectedEndpoint]);

  useEffect(() => {
    if (!remoteOnline || !state.host) return;
    setActivePage("devices");
    setDevicesExpanded(true);
    const currentEndpoint = endpointKey(state.host, state.port);
    if (selectedEndpoint !== currentEndpoint) setSelectedEndpoint(currentEndpoint);
  }, [remoteOnline, selectedEndpoint, state.host, state.port]);

  useEffect(() => {
    if (!selectedEndpoint || remoteOnline || selectedDevice) return;
    setSelectedEndpoint("");
  }, [remoteOnline, selectedDevice, selectedEndpoint]);

  useEffect(() => {
    if (!deviceContextMenu) return undefined;
    const closeMenu = () => setDeviceContextMenu(null);
    const closeOnEscape = (event) => {
      if (event.key === "Escape") closeMenu();
    };
    window.addEventListener("pointerdown", closeMenu);
    window.addEventListener("blur", closeMenu);
    window.addEventListener("keydown", closeOnEscape);
    return () => {
      window.removeEventListener("pointerdown", closeMenu);
      window.removeEventListener("blur", closeMenu);
      window.removeEventListener("keydown", closeOnEscape);
    };
  }, [deviceContextMenu]);

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

  const showDevices = () => {
    if (state.role !== "controller") setRole("controller");
    setActivePage("devices");
    setDevicesExpanded((expanded) => activePage === "devices" ? !expanded : true);
    sendCommand("requestRecentDevices");
  };

  const selectDevice = (device) => {
    if (state.role !== "controller") setRole("controller");
    setActivePage("devices");
    setSelectedEndpoint(endpointKey(device.host, device.port));
    setDeviceContextMenu(null);
  };

  const showDeviceContextMenu = (event, device) => {
    if (!device.recentId) return;
    event.preventDefault();
    setDeviceContextMenu({
      deviceId: device.recentId,
      x: Math.min(event.clientX, window.innerWidth - 180),
      y: Math.min(event.clientY, window.innerHeight - 54),
    });
  };

  const errors = [state.error, state.recentDeviceError, state.applicationSettingsError, state.fileTransferError].filter(Boolean);

  return (
    <main className="dashboard-shell">
      <aside className="app-sidebar">
        <div className="app-brand"><img src="/app-icon.png" alt="" /><div><strong>winRemote</strong><small>Control</small></div></div>
        <nav className="app-nav">
          <button className={`device-nav-toggle ${activePage === "devices" ? "active" : ""}`} aria-expanded={devicesExpanded} onClick={showDevices}><Icon name="devices" /><span>我的设备</span><span className={`nav-chevron ${devicesExpanded ? "is-expanded" : ""}`}><Icon name="chevron" /></span></button>
          <div className={`sidebar-device-list ${devicesExpanded ? "is-expanded" : ""}`} aria-hidden={!devicesExpanded}>
            <div className="sidebar-device-list-inner">
              {mergedDevices.map((device) => {
                const selected = endpointKey(device.host, device.port) === selectedEndpoint;
                const status = device.online ? "局域网在线" : formatRecentTime(device.lastConnectedAtMs);
                return (
                  <button
                    key={device.key}
                    className={`sidebar-device-row ${selected ? "is-selected" : ""}`}
                    title={`${device.name || "Windows 设备"}\n${device.host}:${device.port}\n${status}`}
                    onClick={() => selectDevice(device)}
                    onContextMenu={(event) => showDeviceContextMenu(event, device)}
                  >
                    <i className={device.online ? "is-online" : ""} />
                    <span><strong>{device.name || "Windows 设备"}</strong><small>{status}</small></span>
                  </button>
                );
              })}
              {mergedDevices.length === 0 && <div className="sidebar-device-empty"><span>暂无设备</span><small>正在搜索局域网</small></div>}
            </div>
          </div>
          <button className={activePage === "assist" ? "active" : ""} onClick={() => setActivePage("assist")}><Icon name="assist" /><span>远程协助</span></button>
        </nav>
        <button className={`settings-nav-button ${activePage === "settings" ? "active" : ""}`} onClick={() => { setActivePage("settings"); sendCommand("requestApplicationSettings"); }}><Icon name="settings" /><span>设置</span></button>
        <div className="sidebar-foot"><span className="privacy-dot" /><p>仅局域网连接</p></div>
      </aside>

      <div className={`dashboard-main ${activePage === "settings" ? "is-settings-page" : ""}`}>
        <header className="dashboard-topbar" onPointerDown={(event) => {
          if (event.button === 0 && event.target.closest("[data-window-control]") === null) sendCommand("beginMainWindowDrag");
        }}>
          <div className="dashboard-title-actions" data-window-control>
            <div className={`discovery-state ${state.lanDiscoveryError ? "limited" : ""}`}><i />{state.lanDiscoveryError ? "广播受限" : "局域网已就绪"}</div>
            <MainWindowControls />
          </div>
        </header>
        {activePage === "settings" ? (
          <SettingsPage state={state} />
        ) : remoteOnline ? (
          <ConnectedSession state={state} wallpaperUrl={wallpaperUrl} onDisconnect={() => sendCommand("disconnectSession")} onOpenTerminal={() => sendCommand("openCurrentTerminal")} onOpenFileTransfer={() => sendCommand("openCurrentFileTransfer")} />
        ) : activePage === "devices" ? (
          <DevicesPage
            selectedDevice={selectedDevice}
            busy={busy}
            connectDevice={beginConnection}
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
		{state.pairingRequest && <PairingRequestModal request={state.pairingRequest} />}
		{state.incomingTerminalRequest && <TerminalRequestModal request={state.incomingTerminalRequest} />}
		{state.terminalState.state === "Running" && state.role === "controlled" && <div className="terminal-running-banner"><Icon name="terminal" /><div><strong>远程终端正在运行</strong><small>PowerShell 使用当前登录用户权限</small></div><button onClick={() => sendCommand("closeTerminal")}>停止终端</button></div>}
		{["Ready", "Reconnecting"].includes(state.fileTransferState.state) && state.role === "controlled" && <div className="terminal-running-banner file-transfer-running-banner"><Icon name="fileTransfer" /><div><strong>{state.fileTransferState.state === "Reconnecting" ? "文件传输已暂停" : "文件传输正在进行"}</strong><small>{state.fileTransferState.activeTasks > 0 ? `${state.fileTransferState.activeTasks} 个活动任务` : "控制端可以浏览并复制已授权会话中的文件"}</small></div><button onClick={() => sendCommand("stopCurrentFileTransfer")}>停止传输</button></div>}
        {deviceContextMenu && <div className="device-context-menu" style={{ left: deviceContextMenu.x, top: deviceContextMenu.y }} onPointerDown={(event) => event.stopPropagation()}><button onClick={() => { sendCommand("removeRecentDevice", { deviceId: deviceContextMenu.deviceId }); setDeviceContextMenu(null); }}><Icon name="trash" />移除最近记录</button></div>}
        {errors.length > 0 && <div className="error-stack">{errors.map((error, index) => <p key={`${index}-${error}`}>{error}</p>)}</div>}
      </div>
    </main>
  );
}

export function DesktopPage() {
  const state = useNativeState();
  const previewSlotRef = useRef(null);
  const [elapsedSeconds, setElapsedSeconds] = useState(0);
  const reconnecting = state.webrtcState === "Reconnecting";
  const retrying = ["Connecting", "AwaitingApproval", "Negotiating"].includes(state.webrtcState);
  const sessionEnded = ["Disconnected", "Failed"].includes(state.webrtcState);
  const shutdownTimedOut = state.webrtcState === "ShutdownTimedOut";
  const sessionUnavailable = reconnecting || retrying || sessionEnded || state.webrtcState === "Stopping" || shutdownTimedOut;
  const qualityPresets = {
    // Frame rate is an independent setting; presets must not carry "fps" or
    // they would reset the user's chosen frame rate on every desktop mount.
    auto: { width: 1280, height: 720, bitrateKbps: 3000 },
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
    sendCommand("requestClipboardSyncState");
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
        <DesktopWindowControls clipboardSync={state.clipboardSync} />
      </header>
      <section className="desktop-stage">
        <div ref={previewSlotRef} className="native-preview-slot desktop-slot" />
        {sessionUnavailable && (
          <div className={`desktop-disconnected ${reconnecting || retrying ? "is-recovering" : ""}`}>
            <div className="disconnect-orbit"><span /><span /><b>!</b></div>
            <strong>{reconnecting ? "正在恢复连接" : retrying ? "正在重新连接" : shutdownTimedOut ? "会话关闭超时" : state.webrtcState === "Stopping" ? "正在结束会话" : "远程连接已断开"}</strong>
            <p>{reconnecting
              ? "正在尝试恢复当前会话，恢复期间已暂停键盘和鼠标输入。"
              : retrying
                ? state.webrtcState === "AwaitingApproval" ? "已重新联系被控端，正在等待对方确认。" : "正在重新建立安全连接，请稍候。"
                : shutdownTimedOut ? "部分系统组件未能及时关闭，请重新启动程序。"
                : state.webrtcState === "Stopping" ? "正在安全释放输入、视频和网络资源。" : "当前会话已经结束，可以重新申请控制或返回主界面。"}</p>
            {retrying && <button className="outline-button" onClick={() => sendCommand("disconnectSession")}>取消重连</button>}
            {sessionEnded && (
              <div className="desktop-recovery-actions">
                <button className="outline-button" onClick={() => sendCommand("closeDesktop")}>返回主界面</button>
                {state.sessionError?.retryable !== false && (
                  <button className="primary-button" onClick={() => sendCommand("retryLastConnection")}>重新连接</button>
                )}
              </div>
            )}
          </div>
        )}
      </section>
      {state.error && <p className="desktop-error">{state.error}</p>}
      {state.clipboardSyncError && <p className="desktop-error clipboard-error">{state.clipboardSyncError}</p>}
    </main>
  );
}
