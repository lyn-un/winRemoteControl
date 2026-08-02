import { sendCommand } from "../bridge/nativeBridge";

export default function DesktopWindowControls({ clipboardSync }) {
  const statusText = clipboardSync.active
    ? "剪贴板同步"
    : clipboardSync.status === "paused"
      ? "同步已暂停"
      : clipboardSync.enabled ? "剪贴板不可用" : "剪贴板已关闭";
  return (
    <div className="desktop-title-right" data-window-control>
      <button
        className={`clipboard-sync-button ${clipboardSync.active ? "is-active" : ""}`}
        disabled={!clipboardSync.available}
        title={statusText}
        onClick={() => sendCommand("setClipboardSyncEnabled", { enabled: !clipboardSync.enabled })}
      >
        <svg viewBox="0 0 24 24" aria-hidden="true">
          <rect x="7" y="5" width="12" height="16" rx="2" />
          <path d="M9 5V3h8v2M5 17H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h1" />
        </svg>
        <span>{statusText}</span>
        <i className={clipboardSync.enabled ? "is-on" : ""}><b /></i>
      </button>
      <div className="control-center-wrap">
        <button
          className="control-center-button"
          onClick={(event) => {
            const rect = event.currentTarget.getBoundingClientRect();
            sendCommand("showControlCenterMenu", {
              x: Math.round(rect.left),
              y: Math.round(rect.bottom + 4),
            });
          }}
        >
          <span className="slider-icon" />
          控制中心
        </button>
      </div>
      <button className="window-button" aria-label="最小化" onClick={() => sendCommand("minimizeDesktopWindow")}>—</button>
      <button className="window-button" aria-label="最大化" onClick={() => sendCommand("toggleMaximizeDesktopWindow")}>□</button>
      <button className="window-button is-close" aria-label="关闭" onClick={() => sendCommand("closeDesktop")}>×</button>
    </div>
  );
}
