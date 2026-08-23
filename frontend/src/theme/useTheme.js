import { useEffect, useState } from "react";
import { THEMES } from "./themeCatalog";
import { getCurrentThemeId, requestThemeChange, subscribeToTheme } from "./themeBridge";

export function useTheme() {
  const [themeId, setThemeId] = useState(getCurrentThemeId);

  useEffect(() => subscribeToTheme(setThemeId), []);

  return {
    themeId,
    themes: THEMES,
    setTheme: requestThemeChange,
  };
}
