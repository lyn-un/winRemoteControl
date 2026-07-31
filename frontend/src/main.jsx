import React from "react";
import { createRoot } from "react-dom/client";
import { getViewMode } from "./bridge/nativeBridge";
import DashboardPage from "./pages/DashboardPage";
import DesktopPage from "./pages/DesktopPage";
import "./styles.css";

const Page = getViewMode() === "desktop" ? DesktopPage : DashboardPage;

createRoot(document.getElementById("root")).render(<Page />);
