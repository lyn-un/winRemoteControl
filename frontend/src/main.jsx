import React from "react";
import { createRoot } from "react-dom/client";
import { getViewMode } from "./bridge/nativeBridge";
import DashboardPage from "./pages/DashboardPage";
import DesktopPage from "./pages/DesktopPage";
import TerminalPage from "./pages/TerminalPage";
import "./styles.css";

const viewMode = getViewMode();
const Page = viewMode === "desktop" ? DesktopPage : viewMode === "terminal" ? TerminalPage : DashboardPage;

createRoot(document.getElementById("root")).render(<Page />);
