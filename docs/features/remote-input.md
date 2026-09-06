# 远程输入

## 范围与原则

远程输入由 C++ 负责采集、路由、传输、校验和 Win32 注入。React 只显示键盘设置和捕获状态，不接触物理按键事件、输入序列或 `SendInput()`。

当前支持：

- 鼠标移动、左右/中键、扩展键和滚轮。
- 物理键盘虚拟键、扫描码、扩展键和自动重复。
- 线上 Unicode 文本消息与被控端 Unicode 注入；原生键盘捕获模式下的 IME 提交仍需补齐并重新验证。
- 远控窗口键盘捕获、本地快捷键和功能快捷键。
- 双击坐标锚定和按住按钮期间的有序拖拽。

输入只在控制端角色、`Streaming` 状态、双方能力协商成功且最终权限包含 `inputControl` 时启用。Windows 安全注意序列 `Ctrl+Alt+Delete` 不可由普通应用捕获或模拟，本项目明确拒绝把它配置为快捷键。

## 总体链路

```text
物理鼠标/键盘
  -> KVideoRenderWidget / KWindowsRemoteKeyboardCaptureAdapter
  -> KRemoteKeyboardRoutingService / KSessionViewModel
  -> KWebRtcPeer 输入路由
  -> input 或 input-realtime DataChannel
  -> KInputInjector 顺序与生命周期校验
  -> KWindowsInputInjector
  -> SendInput()
  -> Windows 目标窗口
```

主要所有权：

| 组件 | 职责 |
| --- | --- |
| `KVideoRenderWidget` | 视频坐标映射、鼠标事件、双击锚定、捕获请求 |
| `KRemoteKeyboardRoutingService` | 物理键盘捕获、修饰键暂存、快捷键路由和远端按键释放 |
| `KKeyboardSettingsService` | 快捷键校验、数量限制和持久化编排 |
| `KMouseInputRoutingState` | 空闲移动与拖拽移动的 DataChannel 选择 |
| `KWebRtcPeer` | 有界发送队列、移动合并和通道发送 |
| `KInputInjector` | 接收序号、迟到消息过滤和输入生命周期 |
| `KWindowsInputInjector` | 最终 Win32 `SendInput()` 调用和已按下状态清理 |

## 键盘捕获

### 启用条件

单击有效视频区域时请求捕获。以下条件必须同时满足：

- 会话状态为 `Streaming`。
- 协商结果有效并支持键盘。
- 最终权限包含 `inputControl`。
- 远控窗口和原生视频窗口句柄有效。
- 远控窗口是当前前台窗口。

失焦、窗口隐藏或最小化、重连、断开、generation 变化、权限撤销、能力失效和窗口销毁都会停止捕获，并为已经发往远端但尚未释放的按键补发 `KeyUp`。

### 原生低级钩子

`KWindowsRemoteKeyboardCaptureAdapter` 使用 `WH_KEYBOARD_LL` 捕获物理键盘：

- 物理事件由 C++ 路由并在捕获期间从本机输入链中吞掉。
- 带 `LLKHF_INJECTED` 或 `LLKHF_LOWER_IL_INJECTED` 的事件不作为物理输入处理，继续传递给 Windows。
- 本地快捷键回放使用专用 `dwExtraInfo` 标记及 `SendInput()`，不会再次被路由到远端。
- Qt 视频控件在原生路由启用时停止重复转发普通 `QKeyEvent` 和 `QInputMethodEvent`。这避免同一物理键被发送两次，但也意味着当前原生捕获模式尚未把 IME commit string 接回 Unicode 文本通道；协议和被控端注入能力仍保留，不能据此宣称远控窗口中的 IME 已完成回归。

### 修饰键前缀

Ctrl、Alt、Shift 和 Win 的 `KeyDown` 先暂存，直到主键到来后才能判断组合归属：

- 命中功能快捷键：整个组合只执行本机 C++ Action。
- 命中“仅控制端响应”：整个组合通过带标记的 `SendInput()` 回放给本机 Windows，不发送远端。
- 未命中：按原顺序先发送暂存修饰键，再发送主键。
- 修饰键单独释放：先刷新暂存状态，再发送对应远端 `KeyUp`。
- 发送远端鼠标按钮或滚轮前刷新暂存修饰键，保证 `Ctrl+单击`、`Shift+单击` 等行为成立。

被本地快捷键消费的按键会进入 draining 状态，直到相应物理按键全部释放，防止释放事件泄漏到本地或远端。

### 内置与用户快捷键

| 功能 | 默认组合 | 配置 |
| --- | --- | --- |
| 释放键盘捕获 | `Ctrl+Alt+Enter` | 固定，不可删除或修改 |
| 切换远控全屏 | `Ctrl+Alt+F` | 可修改 |
| 仅控制端响应 | 默认空 | 最多 32 个 |

快捷键必须包含至少一个修饰键和一个非修饰主键。重复项、功能冲突、裸修饰键及 `Ctrl+Alt+Delete` 会被拒绝。录制使用临时原生钩子，`Escape` 取消，10 秒无有效组合自动超时。

设置以规范化名称持久化，例如 `Ctrl+Alt+F`。左右 Ctrl/Alt/Shift/Win 在配置比较时归一化，实际发往远端的扫描码和扩展键信息仍被保留。QSettings 键包括：

```text
keyboard/schemaVersion
keyboard/localOnlyShortcuts
keyboard/toggleFullscreen
```

## 鼠标移动、按钮与双击

### 坐标和按钮注入

视频控件把可见视频区域坐标映射为远端桌面坐标。被控端每条按钮消息使用一次 `SendInput(2, ...)` 连续提交：

1. 绝对坐标移动。
2. 对应按钮的 `Down` 或 `Up`。

只有 `SendInput()` 完整插入两条 `INPUT` 时才更新本地已按下按钮集合。断开或输入生命周期结束时会尝试释放仍处于按下状态的键盘和鼠标按钮。

### 双击锚定

第一次普通点击保持即时发送，不为等待双击增加延迟。当 Qt 已识别本地双击时：

- 第一次 `Down/Up` 使用正常映射坐标。
- 第二次 `Down` 固定到第一次已完成点击的远端坐标。
- 随后的第二次 `Up` 使用同一锚点。
- 线上仍是兼容旧端的四条普通按钮消息：`Down -> Up -> Down -> Up`。

按钮不同、缓存无效或生命周期已经变化时退回当前映射坐标。画面尺寸变化、失焦、隐藏、清空画面和会话暂停都会清理双击状态。

## 有序拖拽与 DataChannel

空闲鼠标移动追求低延迟，可以被更新状态覆盖；按钮边界和拖拽要求顺序完整。双方通过可选能力 `orderedMouseDrag` 协商新路由：

- 未按按钮时，移动继续走无序、零重传的 `input-realtime`。
- `Down` 前清除尚未发送的实时移动，然后通过可靠有序的 `input` 发送。
- 任意按钮按住期间，移动改走 `input`。
- 同一按钮状态区间内的连续拖拽移动可以合并，但合并键带有状态 epoch，不能越过 `Down/Up`。
- `Up` 在可靠拖拽移动之后发送，完成后恢复实时移动。
- 多按钮状态以集合维护；只有所有按钮释放后才恢复实时通道。

被控端在协商启用后维护成功注入的按钮集合。按钮按住期间迟到的实时移动会被丢弃，但其序号水位仍推进，防止旧移动稍后重新注入。可靠拖拽移动正常注入。通道关闭、重连、generation 变化或输入释放会清空路由状态。

缺少 `orderedMouseDrag` 字段、只有单端支持或未启用实时输入时，按兼容路径处理，不阻止基础远控，也不修改旧 Windows/Android 客户端行为。

## 人工桌面兼容问题：已定位，暂缓处理

记录日期：2026-09-05。用户决定本轮只归档结论，暂停继续排查和兼容实现；不新增驱动、服务、自动提权或强制管理员启动。

### 已获得的实测证据

以下来自用户在另一台电脑提供的排查报告，本次文档更新未重新执行实机测试：

- 人工桌面 DPPlayer 以管理员权限运行，并把播放器窗口嵌入普通权限 Explorer 桌面。
- 实体鼠标操作正常；独立测试程序的普通权限模拟输入出现“只收到抬起、收不到按下”。
- 结束 DPPlayer 后桌面双击恢复，播放器自动重启后再次失效。
- 以管理员权限发送相同输入可以成功。

这些对照支持“跨权限桌面窗口嵌入导致普通权限输入异常”的定位，而不是“Windows 桌面不支持 SendInput”。目前没有证据证明 HY.Hooks 吞掉了这些点击；此前依据导入表、窗口采样或钩子时段作出的排除结论不能替代运行时证据。

尚未定位 Windows 内部具体在哪个检查点丢失按下事件。`SendInput()` 返回成功不代表目标窗口处理成功；旧日志中约 300 ms 延迟或缺失抬起的现象，也不能直接归为同一机制。

### Sunshine 参考与后续候选方案（均未实现）

- Moonlight 是控制端；Sunshine 是实际执行注入的被控端。Sunshine 的 Windows 服务复制 LocalSystem Token，绑定当前交互会话，并启动 Sunshine 进程；传统键鼠路径仍使用 `SendInput()`。参考[服务源码](https://github.com/LizardByte/Sunshine/blob/master/tools/sunshinesvc.cpp)和[版本输入文档](https://docs.lizardbyte.dev/projects/sunshine/v2025.921.42242/platform_2windows_2input_8cpp.html)。
- 查询时的 Sunshine `master` 文档还提供可选虚拟 HID 键鼠路径，驱动不可用时保留 SendInput 回退；这不代表所有已发布版本都要求键鼠驱动。参考[官方安装与输入说明](https://github.com/LizardByte/Sunshine/blob/master/docs/getting_started.md)。上述链接中的开发分支内容可能继续变化。
- 若未来恢复此任务，优先评估被控端由用户显式确认 UAC 的“以管理员权限重新启动”入口，并重新验证人工桌面场景；不要自动重启或静默提权。
- 更长期可评估普通权限 UI 与独立高权限输入辅助进程的隔离方案；服务、IPC 身份校验和权限边界需单独设计。不为此问题直接引入驱动，也不默认让整个 UI、终端和文件传输进程运行于 SYSTEM 权限。

当前只记录兼容限制，不宣称本项目已修复此场景。临时可由用户自行关闭人工桌面规避；是否调整第三方程序权限由用户决定。

## UI Bridge

键盘设置由 C++ 作为唯一真相。Dashboard Bridge 支持：

```text
requestKeyboardSettings
beginKeyboardShortcutRecording
cancelKeyboardShortcutRecording
removeLocalKeyboardShortcut
restoreDefaultKeyboardShortcuts
```

新增本地快捷键和修改全屏快捷键均通过原生录制结果提交，不接受 React 自行构造的物理按键流。C++ 向前端发布 `keyboardSettingsChanged`、`keyboardSettingsError` 和 `shortcutRecordingStateChanged`。

## 诊断与限制

常规输入延迟使用 latency trace 的 `input_send/recv`、`input_route`、`inject_begin/end`、`input_drop` 和 round-trip 事件。桌面目标窗口、低级鼠标钩子及 `SendInput()` 细节使用开发参数 `--desktop-input-trace`，具体字段见[日志与诊断](../development/logging-and-diagnostics.md)。

`SendInput()` 生成的事件带 Windows 注入标志。第三方低级键鼠钩子、安全软件、完整性边界或安全桌面可能过滤此类事件；本项目不绕过 Windows 安全桌面，也不把 API 返回成功等同于目标窗口已经处理输入。

当前自动化覆盖纯逻辑、Codec、路由状态、设置和原生隔离测试：

```text
wrc_keyboard_settings_tests
wrc_remote_keyboard_routing_tests
wrc_mouse_input_routing_state_tests
wrc_remote_mouse_click_tests
wrc_input_injector_tests
wrc_windows_desktop_input_diagnostics_tests
```

原生双击窗口测试 `wrc_windows_input_double_click_test` 默认以返回码 77 跳过；只有显式设置 `WRC_RUN_INPUT_E2E=1` 才会移动本机鼠标并运行。桌面快捷方式、第三方动态壁纸、系统组合键、IME，以及不同显示缩放/多显示器环境仍需在双机受控环境中验证。
