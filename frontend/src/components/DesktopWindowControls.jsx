import { sendCommand } from "../bridge/nativeBridge";

export default function DesktopWindowControls() {
  return (
    <div className="desktop-title-right" data-window-control>
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
