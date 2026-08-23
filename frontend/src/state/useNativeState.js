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
  const [sessionError, setSessionError] = useState(null);
  const [lanDiscoveryError, setLanDiscoveryError] = useState("");
  const [recentDeviceError, setRecentDeviceError] = useState("");
  const [applicationSettings, setApplicationSettings] = useState(null);
  const [applicationSettingsError, setApplicationSettingsError] = useState("");
  const [clipboardSync, setClipboardSync] = useState({
    enabled: true,
    available: false,
    active: false,
    status: "unavailable",
  });
  const [clipboardSyncError, setClipboardSyncError] = useState("");
	const [sessionCapabilities, setSessionCapabilities] = useState(null);
  const [incomingAccessRequest, setIncomingAccessRequest] = useState(null);
	const [pairingRequest, setPairingRequest] = useState(null);
	const [deviceAuthentication, setDeviceAuthentication] = useState(null);
	const [trustedDevices, setTrustedDevices] = useState([]);
	const [trustedDeviceError, setTrustedDeviceError] = useState("");
	const [securityMigrationNotice, setSecurityMigrationNotice] = useState("");
	const [sessionPermissions, setSessionPermissions] = useState([]);
	const [incomingTerminalRequest, setIncomingTerminalRequest] = useState(null);
	const [terminalState, setTerminalState] = useState({ state: "Closed", available: false, status: "" });
	const [terminalFrontendSupport, setTerminalFrontendSupport] = useState({ supported: false, reason: "" });
	const [terminalError, setTerminalError] = useState("");
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
      setSessionError(null);
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

      if (message.type === "sessionError") {
        setSessionError({
          domain: message.domain || "unknown",
          code: message.code || "unknown",
          stage: message.stage || "unknown",
          retryable: Boolean(message.retryable),
        });
        setError(message.message || "远程会话发生错误");
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
          themeId: message.themeId || "nordic-mist",
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

      if (message.type === "applicationThemeError") {
        setApplicationSettingsError(message.message || "主题设置保存失败");
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

		if (message.type === "pairingRequestChanged") {
			setPairingRequest({
				requestId: message.requestId || "",
				deviceName: message.deviceName || "Windows 设备",
				localRole: message.localRole || "",
				verificationCode: message.verificationCode || "",
				controllerFingerprint: message.controllerFingerprint || "",
				controlledFingerprint: message.controlledFingerprint || "",
				tlsProtocol: message.tlsProtocol || "",
				cipherSuite: message.cipherSuite || "",
				permissions: Array.isArray(message.permissions) ? message.permissions : [],
				expiresAtMs: Number(message.expiresAtMs) || Date.now(),
			});
			return;
		}

		if (message.type === "pairingRequestCleared") {
			setPairingRequest((request) => (!request || request.requestId === message.requestId ? null : request));
			return;
		}

		if (message.type === "deviceAuthenticationStateChanged") {
			setDeviceAuthentication({ state: message.state || "", deviceId: message.deviceId || "", fingerprint: message.fingerprint || "", trusted: Boolean(message.trusted) });
			return;
		}

		if (message.type === "trustedDevicesChanged") {
			setTrustedDevices(Array.isArray(message.devices) ? message.devices : []);
			setTrustedDeviceError("");
			return;
		}

		if (message.type === "trustedDeviceError") {
			setTrustedDeviceError(message.message || "可信设备操作失败");
			return;
		}

		if (message.type === "securityMigrationNotice") {
			setSecurityMigrationNotice(message.message || "安全协议已升级，需要重新配对设备。");
			return;
		}

		if (message.type === "sessionPermissionsChanged") {
			setSessionPermissions(Array.isArray(message.permissions) ? message.permissions : []);
			return;
		}

		if (message.type === "incomingTerminalAccessRequest") {
			setIncomingTerminalRequest({ requestId: message.requestId || "", deviceName: message.deviceName || "", sourceAddress: message.sourceAddress || "", expiresAtMs: Number(message.expiresAtMs) || Date.now() });
			return;
		}

		if (message.type === "incomingTerminalAccessRequestCleared") {
			setIncomingTerminalRequest((request) => (!request || request.requestId === message.requestId ? null : request));
			return;
		}

		if (message.type === "terminalStateChanged") {
			setTerminalState({ state: message.state || "Closed", available: Boolean(message.available), status: message.status || "", deviceName: message.deviceName || "", deviceSource: message.deviceSource || "" });
			if (message.state === "Running") setTerminalError("");
			return;
		}

		if (message.type === "terminalFrontendSupportChanged") {
			setTerminalFrontendSupport({ supported: Boolean(message.supported), reason: message.reason || "" });
			return;
		}

		if (message.type === "terminalError") {
			setTerminalError(message.message || "远程终端发生错误");
			return;
		}

      if (message.type === "clipboardSyncStateChanged") {
        setClipboardSync({
          enabled: Boolean(message.enabled),
          available: Boolean(message.available),
          active: Boolean(message.active),
          status: message.status || "unavailable",
        });
        setClipboardSyncError("");
        return;
      }

      if (message.type === "clipboardSyncError") {
        setClipboardSyncError(message.message || "剪贴板同步失败");
        return;
      }

		if (message.type === "sessionCapabilitiesChanged") {
			setSessionCapabilities(message.available ? {
				protocolVersion: Number(message.protocolVersion) || 0,
				videoCodec: message.videoCodec || "",
				maximumWidth: Number(message.maximumWidth) || 0,
				maximumHeight: Number(message.maximumHeight) || 0,
				maximumFps: Number(message.maximumFps) || 0,
				maximumBitrateKbps: Number(message.maximumBitrateKbps) || 0,
				clipboardText: Boolean(message.clipboardText),
				keyboard: Boolean(message.keyboard),
				unicodeText: Boolean(message.unicodeText),
				mouseButtons: Boolean(message.mouseButtons),
				mouseWheel: Boolean(message.mouseWheel),
			} : null);
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
	  sendCommand("requestTrustedDevices");
	  sendCommand("requestTerminalFrontendSupport");
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
    sessionError,
    lanDiscoveryError,
    recentDeviceError,
    applicationSettings,
    applicationSettingsError,
    incomingAccessRequest,
		pairingRequest,
		deviceAuthentication,
		trustedDevices,
		trustedDeviceError,
		securityMigrationNotice,
		sessionPermissions,
		incomingTerminalRequest,
		terminalState,
		terminalFrontendSupport,
		terminalError,
    clipboardSync,
    clipboardSyncError,
		sessionCapabilities,
    fps,
  };
}
