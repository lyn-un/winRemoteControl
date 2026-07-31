export default function SessionMetrics({ signalingState, webrtcState, sessionOpen }) {
  return (
    <div className="metrics">
      <div>
        <label>信令</label>
        <strong>{signalingState}</strong>
      </div>
      <div>
        <label>WebRTC</label>
        <strong>{webrtcState}</strong>
      </div>
      <div>
        <label>控制通道</label>
        <strong>{sessionOpen ? "Ready" : "-"}</strong>
      </div>
    </div>
  );
}
