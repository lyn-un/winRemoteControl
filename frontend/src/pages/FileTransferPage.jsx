import React, { useEffect, useMemo, useState } from "react";
import { sendCommand } from "../bridge/nativeBridge";
import { useFileTransferState } from "../state/useFileTransferState";

function FileIcon({ kind }) {
  const containerKind = ["directory", "drive", "folder", "batch"].includes(kind);
  return <span className={`transfer-file-icon is-${kind || "file"}`} aria-hidden="true">
    {containerKind ? "" : <i />}
  </span>;
}

function formatBytes(value) {
  const bytes = Number(value || 0);
  if (!Number.isFinite(bytes) || bytes <= 0) return bytes === 0 ? "0 B" : "—";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  return `${(bytes / (1024 ** index)).toFixed(index === 0 ? 0 : 1)} ${units[index]}`;
}

function formatTime(value) {
  const timestamp = Number(value || 0);
  if (!timestamp) return "—";
  return new Intl.DateTimeFormat("zh-CN", {
    year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit",
  }).format(timestamp);
}

function WindowControls({ onClose }) {
  return <div className="transfer-window-controls" data-window-control>
    <button title="最小化" onClick={() => sendCommand("minimizeFileTransferWindow")}><svg viewBox="0 0 24 24"><path d="M6 16h12" /></svg></button>
    <button title="最大化或还原" onClick={() => sendCommand("toggleMaximizeFileTransferWindow")}><svg viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="1" /></svg></button>
    <button className="is-close" title="关闭" onClick={onClose}><svg viewBox="0 0 24 24"><path d="m7 7 10 10M17 7 7 17" /></svg></button>
  </div>;
}

function FilePane({ side, pane, selected, setSelected }) {
  const [path, setPath] = useState("");
  useEffect(() => setPath(pane.displayPath || ""), [pane.displayPath]);
	useEffect(() => setSelected(new Set()), [pane.listingId, setSelected]);
  const sortedEntries = useMemo(() => [...pane.entries].sort((left, right) => {
    const leftFolder = left.kind === "directory" || left.kind === "drive";
    const rightFolder = right.kind === "directory" || right.kind === "drive";
    if (leftFolder !== rightFolder) return leftFolder ? -1 : 1;
    return String(left.name || "").localeCompare(String(right.name || ""), "zh-CN", { sensitivity: "base" });
  }), [pane.entries]);

  const navigate = (entry) => {
    if (!entry.navigable) return;
    setSelected(new Set());
    sendCommand("navigateFilePane", {
      pane: side,
      listingId: pane.listingId,
      targetEntryId: entry.entryId,
    });
  };
  const toggle = (entryId) => {
    setSelected((current) => {
      const next = new Set(current);
      if (next.has(entryId)) next.delete(entryId);
      else next.add(entryId);
      return next;
    });
  };
  const navigatePath = (event) => {
    event.preventDefault();
    const target = path.trim() || pane.displayPath;
    if (target) sendCommand("navigateFilePaneByPath", { pane: side, path: target });
  };

  return <section className="file-pane">
    <header>
      <div><span className={`pane-role ${side}`}>{side === "local" ? "本机" : "远端"}</span><strong>{side === "local" ? "这台电脑" : "被控设备"}</strong></div>
      <button title="刷新" onClick={() => sendCommand("refreshFilePane", { pane: side })}>↻</button>
    </header>
    <form className="file-address-bar" onSubmit={navigatePath}>
      <button type="button" disabled={!pane.canGoUp} title="返回上级" onClick={() => sendCommand("navigateFilePaneUp", { pane: side, listingId: pane.listingId })}>↑</button>
      <input value={path} onFocus={() => setPath(pane.displayPath)} onChange={(event) => setPath(event.target.value)} placeholder={pane.displayPath || "选择磁盘"} aria-label={`${side === "local" ? "本机" : "远端"}路径`} />
      <button type="submit">前往</button>
    </form>
    <div className="file-table-head"><span /><span>名称</span><span>修改时间</span><span>类型</span><span>大小</span></div>
    <div className={`file-table-body ${pane.loading ? "is-loading" : ""}`}>
      {pane.loading && <div className="file-pane-status"><span className="transfer-spinner" />正在读取目录</div>}
      {!pane.loading && sortedEntries.map((entry) => <div className={`file-row ${selected.has(entry.entryId) ? "is-selected" : ""} ${!entry.transferable ? "is-disabled" : ""}`} key={entry.entryId} onDoubleClick={() => navigate(entry)}>
        <input type="checkbox" checked={selected.has(entry.entryId)} disabled={!entry.transferable} onChange={() => toggle(entry.entryId)} />
        <button className="file-name" disabled={!entry.navigable} title={entry.name} onClick={() => navigate(entry)}><FileIcon kind={entry.kind} /><span>{entry.name}</span></button>
        <span>{formatTime(entry.modifiedAtMs)}</span>
        <span>{entry.kind === "directory" ? "文件夹" : entry.kind === "drive" ? "磁盘" : entry.extension || "文件"}</span>
        <span>{entry.kind === "file" ? formatBytes(entry.sizeBytes) : "—"}</span>
      </div>)}
      {!pane.loading && sortedEntries.length === 0 && <div className="file-pane-status">此位置没有可显示的文件</div>}
    </div>
  </section>;
}

function TaskList({ tasks }) {
  const statuses = {
    Scanning: "正在扫描", Queued: "等待中", Transferring: "传输中", Paused: "已暂停",
    WaitingConflict: "等待选择", Completed: "已完成", Failed: "失败", Cancelled: "已取消",
  };
  return <section className="transfer-tasks">
    <header><div><strong>传输任务</strong><small>{tasks.length ? `${tasks.length} 项` : "暂无任务"}</small></div><button onClick={() => sendCommand("clearCompletedFileTransferTasks")}>清除已完成</button></header>
    <div className="task-list">
      {tasks.map((task) => {
        const total = Number(task.bytesTotal || 0);
        const done = Number(task.bytesTransferred || 0);
        const progress = total > 0 ? Math.min(100, Math.max(0, (done / total) * 100)) : 0;
        return <article className={`transfer-task is-${String(task.status || "queued").toLowerCase()}`} key={task.taskId}>
          <FileIcon kind={task.kind} />
          <div className="task-copy"><strong>{task.displayName || "文件任务"}</strong><small>{task.direction === "download" ? "远端 → 本机" : "本机 → 远端"} · {statuses[task.status] || task.status}</small><div className="task-progress"><i style={{ width: `${progress}%` }} /></div></div>
          <div className="task-metric"><strong>{progress.toFixed(0)}%</strong><small>{formatBytes(done)} / {formatBytes(total)}</small></div>
          <div className="task-actions">
            {task.canPause && <button title="暂停" onClick={() => sendCommand("pauseFileTransferTask", { taskId: task.taskId })}>Ⅱ</button>}
            {task.status === "Paused" && <button title="继续" onClick={() => sendCommand("resumeFileTransferTask", { taskId: task.taskId })}>▶</button>}
            {task.canRetry && <button title="重试" onClick={() => sendCommand("retryFileTransferTask", { taskId: task.taskId })}>↻</button>}
            {["Scanning", "Queued", "Transferring", "Paused", "WaitingConflict"].includes(task.status) && <button title="取消" onClick={() => sendCommand("cancelFileTransferTask", { taskId: task.taskId })}>×</button>}
          </div>
        </article>;
      })}
      {!tasks.length && <div className="empty-task-list"><span>⇄</span><p>选择文件后，传输进度会显示在这里</p></div>}
    </div>
  </section>;
}

function ConflictDialog({ conflict }) {
  const resolve = (resolution, applyToRemaining = false) => {
    sendCommand("resolveFileConflict", { conflictId: conflict.conflictId, resolution, applyToRemaining });
  };
  return <div className="transfer-modal-backdrop"><article className="transfer-conflict-dialog">
    <span className="eyebrow">FILE CONFLICT</span><h2>目标位置已有同名文件</h2><p>{conflict.name || "该文件"}</p>
    <div className="conflict-compare"><div><small>来源文件</small><strong>{formatBytes(conflict.source?.sizeBytes)}</strong><span>{formatTime(conflict.source?.modifiedAtMs)}</span></div><div><small>目标位置</small><strong>已有同名文件</strong><span>覆盖前会保留原文件，直到新文件校验完成</span></div></div>
    <div className="conflict-actions"><button onClick={() => resolve("skip")}>跳过</button><button onClick={() => resolve("keepBoth")}>保留两份</button><button className="primary" onClick={() => resolve("overwrite")}>覆盖</button></div>
    {conflict.applyToRemainingAllowed && <div className="conflict-apply-row"><button onClick={() => resolve("skip", true)}>本批全部跳过</button><button onClick={() => resolve("keepBoth", true)}>本批全部保留两份</button><button onClick={() => resolve("overwrite", true)}>本批全部覆盖</button></div>}
  </article></div>;
}

export default function FileTransferPage() {
  const model = useFileTransferState();
  const [localSelected, setLocalSelected] = useState(new Set());
  const [remoteSelected, setRemoteSelected] = useState(new Set());
  const [closePromptVisible, setClosePromptVisible] = useState(false);
  useEffect(() => {
    const showClosePrompt = () => setClosePromptVisible(true);
    window.addEventListener("wrc:file-transfer-close-prompt", showClosePrompt);
    return () => window.removeEventListener("wrc:file-transfer-close-prompt", showClosePrompt);
  }, []);
  useEffect(() => {
    if (!model.conflict) return;
    const task = model.tasks.find((item) => item.taskId === model.conflict.taskId);
    if (!task || task.status !== "WaitingConflict") model.clearConflict();
  }, [model.tasks, model.conflict]);
  const ready = model.state.state === "Ready" && model.state.available;
  const copy = (sourcePane) => {
    const source = model.panes[sourcePane];
    const destinationPane = sourcePane === "local" ? "remote" : "local";
    const selected = sourcePane === "local" ? localSelected : remoteSelected;
    sendCommand("startFileCopy", {
      sourcePane,
      sourceListingId: source.listingId,
      sourceEntryIds: [...selected],
      destinationListingId: model.panes[destinationPane].listingId,
    });
  };

  return <main className="file-transfer-shell">
    <header className="file-transfer-titlebar" onPointerDown={(event) => {
      if (event.button === 0 && !event.target.closest("[data-window-control]")) sendCommand("beginFileTransferWindowDrag");
    }}>
      <div className="transfer-brand"><img src="/app-icon.png" alt="" /><div><strong>文件传输</strong><small>{model.state.deviceName}{model.state.deviceSource ? ` · ${model.state.deviceSource}` : ""}</small></div></div>
      <div className={`transfer-connection is-${model.state.state.toLowerCase()}`}><i />{model.state.status || model.state.state}</div>
      <WindowControls onClose={() => {
        if (model.activeTasks.length > 0) setClosePromptVisible(true);
        else sendCommand("closeFileTransferWindow");
      }} />
    </header>
    <section className="file-transfer-workspace">
      <FilePane side="local" pane={model.panes.local} selected={localSelected} setSelected={setLocalSelected} />
      <div className="transfer-direction-actions">
        <button disabled={!ready || localSelected.size === 0 || !model.panes.remote.listingId} title="发送到远端" onClick={() => copy("local")}><span>发送</span>→</button>
        <button disabled={!ready || remoteSelected.size === 0 || !model.panes.local.listingId} title="下载到本机" onClick={() => copy("remote")}>←<span>下载</span></button>
      </div>
      <FilePane side="remote" pane={model.panes.remote} selected={remoteSelected} setSelected={setRemoteSelected} />
    </section>
    <TaskList tasks={model.tasks} />
    {!ready && <div className="transfer-state-overlay"><span className="transfer-spinner" /><strong>{model.state.status || "文件通道尚未就绪"}</strong><p>远程桌面会话不会受到影响</p></div>}
    {model.conflict && <ConflictDialog conflict={model.conflict} />}
    {closePromptVisible && <div className="transfer-modal-backdrop"><article className="transfer-close-dialog">
      <span className="eyebrow">ACTIVE TRANSFERS</span><h2>仍有文件正在传输</h2><p>可以关闭窗口并在后台继续，也可以取消全部任务后关闭。</p>
      <div className="conflict-actions"><button onClick={() => setClosePromptVisible(false)}>返回</button><button onClick={() => sendCommand("closeFileTransferWindow")}>后台继续</button><button className="primary danger" onClick={() => { sendCommand("stopCurrentFileTransfer"); sendCommand("closeFileTransferWindow"); }}>取消全部并关闭</button></div>
    </article></div>}
    {model.error && <button className="transfer-error-toast" onClick={model.clearError}>{model.error}</button>}
  </main>;
}
