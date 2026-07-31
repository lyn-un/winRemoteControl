const statusTextMap = {
  Idle: "空闲",
  Capturing: "采集中",
  Stopped: "已停止",
  Error: "错误",
};

export default function CaptureStatus({ captureStatus, webrtcState }) {
  let className = "status";
  if (captureStatus === "Capturing" || webrtcState.includes("connected") || webrtcState === "Streaming") {
    className = "status is-live";
  } else if (captureStatus === "Error") {
    className = "status is-error";
  }

  return (
    <div className={className}>
      <span />
      {statusTextMap[captureStatus] ?? captureStatus}
    </div>
  );
}
