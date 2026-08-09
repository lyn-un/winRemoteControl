import React, { useEffect, useRef, useState } from "react";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import { sendCommand, subscribeToNativeMessages } from "../bridge/nativeBridge";

function encodeBase64(value) {
	const bytes = new TextEncoder().encode(value);
	let binary = "";
	for (const byte of bytes) binary += String.fromCharCode(byte);
	return window.btoa(binary);
}

function decodeBase64(value) {
	const binary = window.atob(value);
	const bytes = new Uint8Array(binary.length);
	for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
	return bytes;
}

export default function TerminalPage() {
	const hostRef = useRef(null);
	const [state, setState] = useState({ state: "Opening", status: "正在准备远程终端" });
	const [error, setError] = useState("");

	useEffect(() => {
		const terminal = new Terminal({
			fontFamily: '"Cascadia Mono", Consolas, monospace',
			fontSize: 14, cursorBlink: true,
			theme: { background: "#101a1e", foreground: "#dce8e8", cursor: "#7fc3cc", selectionBackground: "#315a63" },
		});
		const fit = new FitAddon();
		terminal.loadAddon(fit);
		terminal.open(hostRef.current);
		fit.fit();
		terminal.onData((data) => sendCommand("sendTerminalInput", { dataBase64: encodeBase64(data) }));
		let resizeTimer = 0;
		const resize = () => {
			fit.fit();
			window.clearTimeout(resizeTimer);
			resizeTimer = window.setTimeout(() => sendCommand("resizeTerminal", { columns: terminal.cols, rows: terminal.rows }), 75);
		};
		window.addEventListener("resize", resize);
		const unsubscribe = subscribeToNativeMessages((message) => {
			if (message.type === "terminalOutput" && message.dataBase64) terminal.write(decodeBase64(message.dataBase64));
			else if (message.type === "terminalStateChanged") {
				setState({ state: message.state || "Closed", status: message.status || "", deviceName: message.deviceName || "Windows 设备", deviceSource: message.deviceSource || "" });
				if (message.state === "Running") window.setTimeout(() => { resize(); terminal.focus(); }, 0);
			} else if (message.type === "terminalError") setError(message.message || "远程终端发生错误");
		});
		sendCommand("requestTerminalState");
		return () => { unsubscribe?.(); window.removeEventListener("resize", resize); window.clearTimeout(resizeTimer); terminal.dispose(); };
	}, []);

	const paused = ["Paused", "AwaitingApproval", "Opening"].includes(state.state);
	return <main className="terminal-shell">
		<header className="terminal-titlebar"><div><span>W</span><strong>{state.deviceName || "远程 PowerShell"}</strong><small>{state.deviceSource ? `${state.deviceSource} · ${state.status}` : state.status}</small></div><button onClick={() => sendCommand("closeTerminal")}>停止终端</button></header>
		<section className="terminal-stage"><div ref={hostRef} className="terminal-host" />{paused && <div className="terminal-overlay"><i /><strong>{state.status}</strong><small>终端输入暂时不可用</small></div>}</section>
		{error && <div className="terminal-error">{error}</div>}
	</main>;
}
