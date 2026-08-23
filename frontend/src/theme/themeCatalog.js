export const DEFAULT_THEME_ID = "nordic-mist";

export const THEMES = Object.freeze([
  Object.freeze({
    id: "daylight",
    name: "白天",
    englishName: "Daylight",
    description: "明亮、中性，适合光线充足的环境。",
    preview: Object.freeze({ canvas: "#f9fbfd", sidebar: "#f0f5f9", panel: "#ffffff", accent: "#3f7480" }),
  }),
  Object.freeze({
    id: "midnight",
    name: "黑夜",
    englishName: "Midnight",
    description: "深色、低眩光，适合夜间和暗光环境。",
    preview: Object.freeze({ canvas: "#11191e", sidebar: "#172229", panel: "#202d34", accent: "#79adba" }),
  }),
  Object.freeze({
    id: DEFAULT_THEME_ID,
    name: "北境薄雾",
    englishName: "Nordic Mist",
    description: "当前的雾青色主题，轻盈而克制。",
    preview: Object.freeze({ canvas: "#edf3f5", sidebar: "#dce8eb", panel: "#f9fcfd", accent: "#4c8290" }),
  }),
]);

const themeIds = new Set(THEMES.map((theme) => theme.id));

export function isThemeIdValid(themeId) {
  return typeof themeId === "string" && themeIds.has(themeId);
}

export function normalizeThemeId(themeId) {
  return isThemeIdValid(themeId) ? themeId : DEFAULT_THEME_ID;
}
