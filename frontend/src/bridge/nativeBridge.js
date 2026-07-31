const nativeHost = window.chrome?.webview;

export function getViewMode() {
  return new URLSearchParams(window.location.search).get("view") || "dashboard";
}

export function sendCommand(command, payload = {}) {
  nativeHost?.postMessage({ command, ...payload });
}

export function sendPreviewRect(element) {
  if (!nativeHost || !element) {
    return;
  }

  const rect = element.getBoundingClientRect();
  nativeHost.postMessage({
    command: "previewRectChanged",
    x: Math.round(rect.left),
    y: Math.round(rect.top),
    width: Math.round(rect.width),
    height: Math.round(rect.height),
  });
}

export function subscribeToNativeMessages(listener) {
  if (!nativeHost) {
    return null;
  }

  const handleMessage = (event) => {
    const message = event.data;
    if (typeof message !== "object" || message === null || typeof message.type !== "string") {
      return;
    }
    listener(message);
  };

  nativeHost.addEventListener("message", handleMessage);
  return () => nativeHost.removeEventListener("message", handleMessage);
}

export function isNativeBridgeAvailable() {
  return Boolean(nativeHost);
}
