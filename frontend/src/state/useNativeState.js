import { useEffect, useRef, useState } from "react";
import {
  getViewMode,
  isNativeBridgeAvailable,
  sendCommand,
  subscribeToNativeMessages,
} from "../bridge/nativeBridge";

const emptyNetworkStats = () => ({
  quality: "unknown",
  rttMs: -1,
  jitterMs: -1,
  packetLossRate: 0,
  bitrateKbps: 0,
  fps: 0,
});

export function useNativeState() {
  const [role, setRole] = useState("controller");
  const [host, setHost] = useState("127.0.0.1");
  const [port, setPort] = useState("39000");
  const [captureStatus, setCaptureStatus] = useState("Idle");
  const [signalingState, setSignalingState] = useState("Idle");
  const [webrtcState, setWebrtcState] = useState("Idle");
  const [sessionOpen, setSessionOpen] = useState(false);
  const [deviceInfo, setDeviceInfo] = useState(null);
  const [lanDevices, setLanDevices] = useState([]);
  const [frame, setFrame] = useState(null);
  const [networkStats, setNetworkStats] = useState(emptyNetworkStats);
  const [error, setError] = useState("");
  const [fps, setFps] = useState(0);
  const frameTimes = useRef([]);

  useEffect(() => {
    if (!isNativeBridgeAvailable()) {
      setError("WebView2 桥接不可用");
      return undefined;
    }

    const clearErrorState = () => {
      setError("");
      setCaptureStatus((status) => (status === "Error" ? "Idle" : status));
    };

    return subscribeToNativeMessages((message) => {
      if (message.type === "statusChanged") {
        setCaptureStatus(message.status);
        if (message.status !== "Error") {
          setError("");
        }
        return;
      }

      if (message.type === "signalingChanged") {
        setSignalingState(message.state);
        if (message.state === "Connected" || message.state === "Listening") {
          clearErrorState();
        }
        return;
      }

      if (message.type === "webrtcStateChanged") {
        setWebrtcState(message.state);
        if ([
          "PeerReady",
          "SessionChannelOpen",
          "RemoteVideoTrack",
          "LocalDescriptionSet",
          "RemoteDescriptionSet",
          "Streaming",
          "connected",
          "completed",
        ].includes(message.state)) {
          clearErrorState();
        }
        return;
      }

      if (message.type === "sessionChannelChanged") {
        setSessionOpen(Boolean(message.open));
        if (message.open) {
          clearErrorState();
        } else {
          setDeviceInfo(null);
          setFrame(null);
          setFps(0);
          frameTimes.current = [];
          setNetworkStats(emptyNetworkStats());
        }
        return;
      }

      if (message.type === "deviceInfoChanged") {
        setDeviceInfo({
          computerName: message.computerName || "远程电脑",
          wallpaperMime: message.wallpaperMime || "",
          wallpaperData: message.wallpaperData || "",
          screenWidth: Number(message.screenWidth) || 0,
          screenHeight: Number(message.screenHeight) || 0,
        });
        clearErrorState();
        return;
      }

      if (message.type === "captureError") {
        setError(message.message);
        setCaptureStatus("Error");
        return;
      }

      if (message.type === "lanDevicesChanged") {
        setLanDevices(Array.isArray(message.devices) ? message.devices : []);
        return;
      }

      if (message.type === "lanDiscoveryError") {
        setError(message.message || "局域网设备发现失败");
        return;
      }

      if (message.type === "frameReady") {
        const now = performance.now();
        frameTimes.current = [...frameTimes.current.filter((item) => now - item < 1000), now];
        setFps(frameTimes.current.length);
        setFrame({
          width: message.width,
          height: message.height,
          frameIndex: message.frameIndex,
          timestampMs: message.timestampMs,
        });
        return;
      }

      if (message.type === "networkStatsChanged") {
        setNetworkStats({
          quality: message.quality || "unknown",
          rttMs: Number(message.rttMs),
          jitterMs: Number(message.jitterMs),
          packetLossRate: Number(message.packetLossRate) || 0,
          bitrateKbps: Number(message.bitrateKbps) || 0,
          fps: Number(message.fps) || 0,
        });
      }
    });
  }, []);

  useEffect(() => {
    if (getViewMode() === "dashboard") {
      if (role !== "controller") {
        setLanDevices([]);
      }
      sendCommand("setRole", { role });
    }
  }, [role]);

  return {
    role,
    setRole,
    host,
    setHost,
    port,
    setPort,
    captureStatus,
    signalingState,
    webrtcState,
    sessionOpen,
    deviceInfo,
    lanDevices,
    frame,
    networkStats,
    error,
    fps,
  };
}
