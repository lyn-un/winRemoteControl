# 日志与诊断

## 启动参数

日志默认关闭，推荐通过命令行开启：

| 参数 | 作用 |
| --- | --- |
| `--trace` | 同时开启 session trace 和 latency trace |
| `--session-trace` | 只开启会话、协议和生命周期日志 |
| `--latency-trace` | 只开启采集、视频、输入反馈等延迟日志 |
| `--latency-scenario <static\|mouse\|window>` | 启用延迟日志并标记本次基线场景 |
| `--log-dir <path>` | 指定两类日志目录；相对路径以 exe 目录为基准 |
| `--help` | 显示命令行帮助 |

示例：

```cmd
winRemoteControl.exe --trace
winRemoteControl.exe --session-trace
winRemoteControl.exe --latency-trace
winRemoteControl.exe --latency-scenario mouse
winRemoteControl.exe --trace --log-dir "D:\wrc_logs"
```

建立延迟基线时，两台电脑应使用同一场景标签，并为三个场景分别重新启动一次程序：

```cmd
winRemoteControl.exe --trace --latency-scenario static
winRemoteControl.exe --trace --latency-scenario mouse
winRemoteControl.exe --trace --latency-scenario window
```

每次固定分辨率和帧率：`static` 保持桌面静止，`mouse` 连续移动鼠标，`window` 快速拖动和缩放窗口。控制端会把场景名写入每条延迟日志，可以使用仓库内脚本汇总：

```powershell
.\tools\summarize-latency-trace.ps1 -Scenario mouse
```

脚本输出该标签最后一次运行的端到端 P50、P95、最大值、`lowLatencyRender` 样本和两层帧合并比例。

未指定 `--log-dir` 时写入 exe 同级的 `logs` 目录。目录创建或文件打开失败只通过 `qWarning` 报告，不阻止程序启动。

## 环境变量兼容入口

旧启动脚本仍可使用：

```cmd
set WRC_SESSION_TRACE=1
set WRC_LATENCY_TRACE=1
winRemoteControl.exe
```

命令行负责显式开启；对应参数不存在时才读取环境变量。`WRC_PLAYOUT_DELAY_MAX_MS` 是视频行为选项，不是日志开关。

## 文件命名与轮转

- `session_trace_<role>.log`
- `latency_trace_<side>.log`

`role/side` 根据进程在当次会话中的实际角色变化，因此同一台电脑曾作为不同角色运行时，日志目录里可能同时保留 `controller`、`controlled` 或 `local` 文件。这不表示日志自动同步到对方电脑；每台机器只写自己的本地文件。

单个日志达到 5 MiB 后轮转为同名 `.old`，再创建新文件。Latency trace 使用后台线程批量写入，队列有上限；Session trace 适合低频生命周期事件。

## 常见诊断事件

| 范围 | 重点事件 |
| --- | --- |
| TCP/审批 | `signaling_connect_start/success/failed`、access request/accepted/rejected/timeout |
| 身份安全 | `device_pairing`、`device_authentication`、`pairing_cleanup`、`pairing_rollback_failed`、`security_drop permission_denied`；失败统一带 requestId、generation、domain、code、stage，只记录内部 ID、短指纹、权限和原因 |
| 协商 | SDP/ICE、capabilities sent/negotiated/rejected、DataChannel state |
| 初始化 | WebRTC initialization stage、rollback start/finished/timeout |
| 会话 | `session_recovery_start/success/failed`、`session_end`、`signaling_closed_after_session_end`、shutdown pending components |
| 视频 | capture、`initial_frame_retry`、`h264_encode_no_output`、decode/render、frame coalesced summary；`render_end` 的 `lowLatencyRender` 用于确认 WebRTC 实际渲染模式，`callbackToConvertMs`、`convertToEnqueueMs`、`enqueueToPresentMs`、`callbackToPresentMs` 用于定位控制端阶段 |
| 输入 | `input_send/recv`、`inject_begin/end`、`input_roundtrip`、每 30 个样本的 `input_roundtrip_stats`、会话结束的 `input_roundtrip_summary` |
| 剪贴板 | channel、send/receive/apply/drop；只记录 UUID、大小、序号和原因 |
| 终端 | `terminal_open_requested`、`terminal_approval_pending`、`terminal_started`、`terminal_exited`、`terminal_host_error`、writer cancellation/isolation、`terminal_relay_launch/connected/input/focus/closed` |

排查跨机器问题时，需要分别收集两台电脑同一时间段的日志。日志不会通过 WebRTC 自动传输。

`video_stats` 中的 `jitterTargetEstimateMs` 是 WebRTC 统计出的抖动缓冲目标估计，不应直接解释为当前帧实际等待。判断 0 ms 播放策略是否生效，应查看接收/渲染事件中的 `lowLatencyRender=1`；该字段直接来自 WebRTC 的帧渲染参数。

帧合并分两层统计：`remote_callback_frame_coalesced stage=conversion_queue` 表示 I420→BGRA 转换线程来不及消费，`render_frame_coalesced stage=gui_queue` 表示 GUI 呈现来不及消费。两者的会话结束汇总分别用于区分转换压力与 GUI 排队压力。

## 隐私约束

- 键盘日志不记录虚拟键值、扫描码对应字符或 Unicode 正文。
- 剪贴板日志不记录正文、哈希、文件名或路径。
- 终端日志不记录命令、输出、工作目录或环境变量。
- 发现与最近设备日志不记录设备名称。
- TLS 与配对日志只记录验证方法、协议版本、密码套件、证书校验结果、短设备标识和失败原因；不记录私钥、TLS Exporter、六位配对码、完整证书、完整指纹或信令正文。
- 高频事件应采样或聚合，避免诊断本身明显增加延迟。
