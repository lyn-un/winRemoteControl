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
  const [recentDevices, setRecentDevices] = useState([]);
  const [frame, setFrame] = useState(null);
  const [networkStats, setNetworkStats] = useState(emptyNetworkStats);
  const [error, setError] = useState("");
  const [lanDiscoveryError, setLanDiscoveryError] = useState("");
  const [recentDeviceError, setRecentDeviceError] = useState("");
  const [applicationSettings, setApplicationSettings] = useState(null);
  const [applicationSettingsError, setApplicationSettingsError] = useState("");
  const [incomingAccessRequest, setIncomingAccessRequest] = useState(null);
  const [fps, setFps] = useState(0);
  const frameTimes = useRef([]);
  const settingsInitialized = useRef(false);

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
          if (getViewMode() === "dashboard") {
            sendCommand("requestRecentDevices");
          }
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
        if (Array.isArray(message.devices) && message.devices.length > 0) {
          setLanDiscoveryError("");
        }
        return;
      }

      if (message.type === "lanDiscoveryError") {
        setLanDiscoveryError(message.message || "局域网设备发现失败");
        return;
      }

      if (message.type === "recentDevicesChanged") {
        setRecentDevices(Array.isArray(message.devices) ? message.devices : []);
        setRecentDeviceError("");
        return;
      }

      if (message.type === "recentDeviceError") {
        setRecentDeviceError(message.message || "读取最近设备失败");
        return;
      }

      if (message.type === "applicationSettingsChanged") {
        const settings = {
          remoteAccessEnabled: Boolean(message.remoteAccessEnabled),
          approvalMode: message.approvalMode || "ask",
          approvalTimeoutSeconds: Number(message.approvalTimeoutSeconds) || 30,
          defaultListenPort: Number(message.defaultListenPort) || 39000,
        };
        setApplicationSettings(settings);
        setApplicationSettingsError("");
        if (!settingsInitialized.current) {
          settingsInitialized.current = true;
          setPort(String(settings.defaultListenPort));
        }
        return;
      }

      if (message.type === "applicationSettingsError") {
        setApplicationSettingsError(message.message || "应用设置保存失败");
        return;
      }

      if (message.type === "incomingAccessRequest") {
        setIncomingAccessRequest({
          requestId: message.requestId || "",
          deviceName: message.deviceName || "Windows 设备",
          sourceAddress: message.sourceAddress || "未知地址",
          expiresAtMs: Number(message.expiresAtMs) || Date.now(),
        });
        return;
      }

      if (message.type === "incomingAccessRequestCleared") {
        setIncomingAccessRequest((request) => (
          !request || request.requestId === message.requestId ? null : request
        ));
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
      sendCommand("requestRecentDevices");
      sendCommand("requestApplicationSettings");
    }
  }, []);

  useEffect(() => {
    if (getViewMode() === "dashboard" && applicationSettings) {
      if (role !== "controller") {
        setLanDevices([]);
        setLanDiscoveryError("");
      }
      sendCommand("setRole", { role });
    }
  }, [role, applicationSettings]);

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
    recentDevices,
    frame,
    networkStats,
    error,
    lanDiscoveryError,
    recentDeviceError,
    applicationSettings,
    applicationSettingsError,
    incomingAccessRequest,
    fps,
  };
}
