import { sendCommand, subscribeToNativeMessages } from "../bridge/nativeBridge";
import { DEFAULT_THEME_ID, normalizeThemeId } from "./themeCatalog";

let currentThemeId = DEFAULT_THEME_ID;
let committedThemeId = DEFAULT_THEME_ID;
let pendingThemeId = null;
let unsubscribeNative = null;
const listeners = new Set();

function publishTheme(themeId) {
  listeners.forEach((listener) => listener(themeId));
}

function ensureNativeSubscription() {
  if (unsubscribeNative) return;
  unsubscribeNative = subscribeToNativeMessages((message) => {
    if (message.type === "applicationSettingsChanged") {
      const nextThemeId = normalizeThemeId(message.themeId);
      committedThemeId = nextThemeId;
      if (pendingThemeId && pendingThemeId !== nextThemeId) return;
      pendingThemeId = null;
      applyTheme(nextThemeId);
      return;
    }

    if (message.type === "applicationThemeError" && pendingThemeId) {
      pendingThemeId = null;
      applyTheme(committedThemeId);
    }
  });
}

export function applyTheme(themeId) {
  const normalizedThemeId = normalizeThemeId(themeId);
  document.documentElement.dataset.theme = normalizedThemeId;
  if (currentThemeId !== normalizedThemeId) {
    currentThemeId = normalizedThemeId;
    publishTheme(currentThemeId);
  }
  return normalizedThemeId;
}

export function initializeTheme() {
  const themeId = normalizeThemeId(new URLSearchParams(window.location.search).get("theme"));
  currentThemeId = themeId;
  committedThemeId = themeId;
  document.documentElement.dataset.theme = themeId;
  ensureNativeSubscription();
  return themeId;
}

export function requestThemeChange(themeId) {
  const normalizedThemeId = normalizeThemeId(themeId);
  if (normalizedThemeId === currentThemeId) return;
  pendingThemeId = normalizedThemeId;
  applyTheme(normalizedThemeId);
  sendCommand("updateApplicationTheme", { themeId: normalizedThemeId });
}

export function subscribeToTheme(listener) {
  ensureNativeSubscription();
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function getCurrentThemeId() {
  return currentThemeId;
}
