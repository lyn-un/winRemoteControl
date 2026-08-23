import React from "react";
import { createRoot } from "react-dom/client";
import { getViewMode } from "./bridge/nativeBridge";
import DashboardPage from "./pages/DashboardPage";
import DesktopPage from "./pages/DesktopPage";
import FileTransferPage from "./pages/FileTransferPage";
import { initializeTheme } from "./theme/themeBridge";
import "./styles.css";

initializeTheme();
const viewMode = getViewMode();
const Page = viewMode === "desktop"
  ? DesktopPage
  : viewMode === "fileTransfer" ? FileTransferPage : DashboardPage;

createRoot(document.getElementById("root")).render(<Page />);
