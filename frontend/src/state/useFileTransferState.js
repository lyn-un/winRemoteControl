import { useEffect, useMemo, useRef, useState } from "react";
import { sendCommand, subscribeToNativeMessages } from "../bridge/nativeBridge";

const emptyPane = (pane) => ({
  pane,
  requestId: "",
  listingId: "",
  displayPath: "",
  canGoUp: false,
  loading: true,
  entries: [],
});

export function useFileTransferState() {
  const [state, setState] = useState({
    state: "Opening",
    available: false,
    status: "正在建立安全文件通道",
    deviceName: "远程设备",
    deviceSource: "",
    generation: "0",
  });
  const [panes, setPanes] = useState({
    local: emptyPane("local"),
    remote: emptyPane("remote"),
  });
  const [tasks, setTasks] = useState([]);
  const [conflict, setConflict] = useState(null);
  const [error, setError] = useState("");
  const generationRef = useRef("0");

  useEffect(() => {
    const unsubscribe = subscribeToNativeMessages((message) => {
      if (message.type === "fileTransferStateChanged") {
        const nextState = message.state || "Closed";
        const nextGeneration = String(message.generation || "0");
        const generationChanged = nextGeneration !== generationRef.current;
        generationRef.current = nextGeneration;
        setState({
          state: nextState,
          available: Boolean(message.available),
          status: message.status || "",
          deviceName: message.deviceName || "远程设备",
          deviceSource: message.deviceSource || "",
          generation: nextGeneration,
        });
        if (nextState === "Closed" || generationChanged) {
          setPanes({ local: emptyPane("local"), remote: emptyPane("remote") });
          setTasks([]);
          setConflict(null);
          setError("");
        } else if (nextState === "Ready") {
          setError("");
        }
        return;
      }
      if (message.type === "filePaneChanged") {
        const pane = message.pane === "remote" ? "remote" : "local";
        setPanes((current) => ({
          ...current,
          [pane]: {
            pane,
            requestId: message.requestId || "",
            listingId: message.listingId || "",
            displayPath: message.location?.displayPath || "",
            canGoUp: Boolean(message.location?.canGoUp),
            loading: false,
            entries: Array.isArray(message.entries) ? message.entries : [],
          },
        }));
        return;
      }
      if (message.type === "filePaneLoading") {
        const pane = message.pane === "remote" ? "remote" : "local";
        setPanes((current) => ({
          ...current,
          [pane]: { ...current[pane], loading: true },
        }));
        return;
      }
      if (message.type === "fileTransferSnapshot") {
        if (Array.isArray(message.tasks)) {
          setTasks(message.tasks);
          setConflict(null);
        }
        return;
      }
      if (message.type === "fileTransferTaskChanged") {
        if (!message.task?.taskId) return;
        setTasks((current) => {
          const index = current.findIndex((task) => task.taskId === message.task.taskId);
          if (index < 0) return [message.task, ...current];
          const next = [...current];
          next[index] = message.task;
          return next;
        });
        return;
      }
      if (message.type === "fileTransferTaskRemoved") {
        setTasks((current) => current.filter((task) => task.taskId !== message.taskId));
        return;
      }
      if (message.type === "fileTransferConflictRequested") {
        setConflict(message);
        return;
      }
      if (message.type === "fileTransferClosePromptRequested") {
        window.dispatchEvent(new CustomEvent("wrc:file-transfer-close-prompt"));
        return;
      }
      if (message.type === "fileTransferError") {
        setError(message.message || "文件传输发生错误");
      }
    });
    sendCommand("requestFileTransferSnapshot");
    return unsubscribe || undefined;
  }, []);

  const activeTasks = useMemo(() => tasks.filter((task) =>
    ["Scanning", "Queued", "Transferring", "Paused", "WaitingConflict"].includes(task.status)), [tasks]);

  return {
    state,
    panes,
    tasks,
    activeTasks,
    conflict,
    error,
    clearConflict: () => setConflict(null),
    clearError: () => setError(""),
  };
}
