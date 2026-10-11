# 日志与诊断

## 视频低帧率和模糊诊断（无需 latency trace）

两端使用 `--session-trace --resource-trace`，不要使用 `--trace`；若环境中曾开启
`WRC_LATENCY_TRACE`，先设为 `0`。这样不会因本次诊断切换到 BGRA 渲染。
以下新记录不保存画面、键鼠事件或桌面内容；现有 session 协议日志的行为不变。

发送端的 `capture_watch`、`capture_sink_watch`、`conversion_watch`、
`source_push_watch` 各由一个仅持有计数快照的诊断线程约每秒输出，销毁时输出 `final=1`。
没有帧时仍有心跳；未开启 session trace 时不创建这些线程。
`instance` 区分当前进程中的实例，`elapsedMs` 为实例单调时钟时长；所有计数、
`totalUs` 和 `maxUs` 是实例累计值，帧率用相邻 `success` 差除以时长差计算。
耗时为完成调用的墙钟耗时（不是线程 CPU 时间或 GPU 执行时间）；尚未返回的调用不计入。
尺寸是最近成功帧的尺寸，不是预览图尺寸。

| 阶段 | 计数含义 |
| --- | --- |
| `capture_watch` | 采集调用完成数；`skipped` 是无新桌面帧的超时，不等于丢帧 |
| `capture_sink_watch` | 采集 sink 处理完成数与总耗时；成功可能包含转换池丢帧 |
| `conversion_watch` | BGRA→I420 转换；`skipped` 是缓冲池耗尽丢帧，尺寸为转换输出 |
| `source_push_watch` | 有效 I420 送入 WebRTC sink 的调用；`skipped` 是无 sink；成功不保证编码器收到该帧 |

启动阶段可能重发上一帧，因此 source 计数可能稍高于转换成功数。WebRTC 内部还可能丢帧；
只能结合 `encoder_watch.entered` 定位区段，不把计数差直接归因于网络。
`encoder_watch` 新增累计 `outputBytes`、`keyFrames`、`encodeTotalUs/encodeMaxUs` 和
`ffmpegTotalUs/ffmpegMaxUs`。`measuredOutputBps` 按本地单调时间 `windowUs` 和
成功交给 WebRTC 的 H.264 字节差计算，不包含 RTP/重传开销，不是请求值或网络吞吐量。
总 Encode 耗时包含 FFmpeg 和回调耗时；FFmpeg 耗时仍是主机调用耗时，不是硬件内部耗时。

控制端每秒的 `receive_watch` 保存 WebRTC 累计接收字节、包数、丢包、接收/解码/丢弃帧数、
分辨率、FPS、QP 总和、解码时长及 NACK/PLI 次数，缺失字段写 `unavailable`。
`receive_quality` 复用现有统计器的区间码率、丢包比例（0～1）、RTT、抖动和解码均值；
首个样本的区间码率为 0，原始字段缺失时派生值不能当成有效测量。
跨机器日志时钟未同步，不能直接相减得到端到端延迟。

复测建议：两端重启并使用新的日志目录，先保持桌面静止，再切换/拖动窗口，分别测试
720p 与原画并记下操作时间。结合源尺寸、各阶段帧数、编码实际输出和接收数据定位；
本次仅新增诊断，不改变画质、码率控制、限帧或壁纸预览。

## 启动参数

日志默认关闭，推荐通过命令行开启：

| 参数 | 作用 |
| --- | --- |
| `--trace` | 同时开启 session、latency、resource 和 desktop input trace |
| `--session-trace` | 只开启会话、协议和生命周期日志 |
| `--latency-trace` | 只开启采集、视频、输入反馈等延迟日志 |
| `--resource-trace` | 只开启进程内存、句柄、线程和 GPU 资源日志 |
| `--desktop-input-trace` | 只开启 Windows 被控端桌面输入、目标窗口和低级鼠标钩子诊断 |
| `--latency-scenario <static\|mouse\|window>` | 启用延迟日志并标记本次基线场景 |
| `--log-dir <path>` | 指定所有 trace 日志目录；相对路径以 exe 目录为基准 |
| `--help` | 显示命令行帮助 |
| `--video-renderer <auto\|bgra>` | 接收端默认尝试 I420 GPU 转色，或强制原有 BGRA 路径；非法值报错，不持久化 |

示例：

```cmd
winRemoteControl.exe --trace
winRemoteControl.exe --session-trace
winRemoteControl.exe --latency-trace
winRemoteControl.exe --latency-scenario mouse
winRemoteControl.exe --desktop-input-trace
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

脚本输出该标签最后一次运行的端到端 P50、P95、最大值、`lowLatencyRender` 样本和两层帧合并比例。`allSamples` 保留包含启动过渡的全会话样本，`finalWindow` 来自会话结束时最后 120 个有效样本，用于观察稳定运行阶段。

未指定 `--log-dir` 时写入 exe 同级的 `logs` 目录。目录创建或文件打开失败只通过 `qWarning` 报告，不阻止程序启动。

## 环境变量兼容入口

旧启动脚本仍可使用：

```cmd
set WRC_SESSION_TRACE=1
set WRC_LATENCY_TRACE=1
set WRC_RESOURCE_TRACE=1
winRemoteControl.exe
```

命令行负责显式开启；对应参数不存在时才读取环境变量。`WRC_PLAYOUT_DELAY_MAX_MS` 是视频行为选项，不是日志开关。

## 文件命名与轮转

- `session_trace_<role>.log`
- `latency_trace_<side>.log`
- `resource_trace_<role>.log`
- `desktop_input_trace_controlled.log`

`role/side` 根据进程在当次会话中的实际角色变化，因此同一台电脑曾作为不同角色运行时，日志目录里可能同时保留 `controller`、`controlled` 或 `local` 文件。这不表示日志自动同步到对方电脑；每台机器只写自己的本地文件。

单个日志达到 5 MiB 后轮转为同名 `.old`，再创建新文件。Latency trace 使用后台线程批量写入，队列有上限；Session trace 适合低频生命周期事件。

Desktop input trace 使用独立的 4096 行有界队列和 100 ms 批次。每批采用“打开追加、写入、flush、关闭”，因此进程运行期间即可读取最新记录；队列溢出会写入 `logger_overflow` 和丢弃数量。它只在首次实际鼠标注入时延迟安装观察器，避免未使用输入功能时引入额外系统钩子。

## 常见诊断事件

| 范围 | 重点事件 |
| --- | --- |
| TCP/审批 | `signaling_connect_start/success/failed`、access request/accepted/rejected/timeout |
| 身份安全 | `device_pairing`、`device_authentication`、`pairing_cleanup`、`pairing_rollback_failed`、`security_drop permission_denied`；失败统一带 requestId、generation、domain、code、stage，只记录内部 ID、短指纹、权限和原因 |
| 协商 | SDP/ICE、capabilities sent/negotiated/rejected、DataChannel state |
| 初始化 | WebRTC initialization stage、rollback start/finished/timeout |
| 会话 | `session_recovery_start/success/failed`、`session_end`、`signaling_closed_after_session_end`、shutdown pending components |
| 视频 | capture、`initial_frame_retry`、`h264_encode_no_output`、decode/render、frame coalesced summary；`render_end` 的 `lowLatencyRender` 用于确认 WebRTC 实际渲染模式，`callbackToConvertMs`、`convertToEnqueueMs`、`enqueueToPresentMs`、`callbackToPresentMs` 用于定位控制端阶段 |
| 输入 | `input_send/recv`、`input_route`、`inject_begin/end`、`input_drop`、`input_roundtrip`、每 30 个样本的 `input_roundtrip_stats`、会话结束的 `input_roundtrip_summary` |
| 剪贴板 | channel、send/receive/apply/drop；只记录 UUID、大小、序号和原因 |
| 终端 | `terminal_open_requested`、`terminal_approval_pending`、`terminal_started`、`terminal_exited`、`terminal_host_error`、writer cancellation/isolation、`terminal_relay_launch/connected/input/focus/closed` |
| 会话安全控制 | `privacy_mode_command`、`privacy_mode_state`、`privacy_mode_restore`、`post_session_action`；记录 generation、requestId、模式、动作和稳定错误码 |

排查跨机器问题时，需要分别收集两台电脑同一时间段的日志。日志不会通过 WebRTC 自动传输。

`video_stats` 中的 `jitterTargetEstimateMs` 是 WebRTC 统计出的抖动缓冲目标估计，不应直接解释为当前帧实际等待。判断 0 ms 播放策略是否生效，应查看接收/渲染事件中的 `lowLatencyRender=1`；该字段直接来自 WebRTC 的帧渲染参数。

帧合并分两层统计：`remote_callback_frame_coalesced stage=conversion_queue` 表示 I420→BGRA 转换线程来不及消费，`render_frame_coalesced stage=gui_queue` 表示 GUI 呈现来不及消费。两者的会话结束汇总分别用于区分转换压力与 GUI 排队压力。

## 会话资源诊断

Resource trace 在初始化、Peer Ready、Capture 启停、Streaming、Peer teardown、回到 Idle/Listening，以及结束后 1/5/20 秒记录快照。每条记录包含本机 PID、generation、阶段、Private Bytes、Working Set、句柄、线程以及 DXGI Dedicated/Shared Usage。延迟样本若遇到新会话，会标记 `stale=1`，只用于诊断，不参与旧会话生命周期决策。

开发构建启用 `WRC_BUILD_RESOURCE_DIAGNOSTICS` 后，探针输出到 `build/cmake-release/diagnostics/Release`，不会复制进部署目录：

```powershell
$env:Path = "$(Resolve-Path .\build\Release);$env:Path"
.\build\cmake-release\diagnostics\Release\wrc_capture_resource_probe.exe --cycles 20
.\build\cmake-release\diagnostics\Release\wrc_encoder_resource_probe.exe --cycles 20 --encoder auto
.\build\cmake-release\diagnostics\Release\wrc_encoder_resource_probe.exe --cycles 5 --encoder h264_mf
.\build\cmake-release\diagnostics\Release\wrc_webrtc_resource_probe.exe --cycles 20
```

完整同 PID 双进程验收使用 Python 自动化驱动，不执行输入注入、隐私屏、显示器关闭或锁屏：

```powershell
python automation\tests\resource_lifecycle.py `
  --executable build\Release\winRemoteControl.exe `
  --cycles 20 --encoder auto --validate
```

JSON 汇总和两端原始资源日志写入 `build/diagnostics/reports`。第一轮作为预热，后续样本计算每轮斜率。

当前机器的隔离结果表明：AMD 的 `h264_mf` 每次销毁后会遗留一个 Event、一个 WaitCompletionPacket 和一个注册表 Key；强制 `h264_mf` 仅保留为诊断入口。`Auto` 在检测到 AMD 显示适配器时使用 `libx264`，其他适配器仍优先 Media Foundation。Desktop Duplication 每次连同 D3D Device 一起重建时，驱动会遗留一个 Mutant 和一个 Section，因此采集适配器只保留进程级 D3D11 Device/Context，仍逐会话释放纹理和 `IDXGIOutputDuplication`，并执行 `ClearState`、`Flush` 和 `Trim`。该平台缓存会保留一组固定的驱动资源，但不得随会话轮次继续增长。

## Windows 桌面输入诊断

`--desktop-input-trace` 用于区分“控制端没有发送”“DataChannel 顺序错误”“`SendInput()` 插入失败”和“Windows/第三方钩子没有继续传递”。日志只在本机被控端产生。

主要阶段：

| 事件 | 含义 |
| --- | --- |
| `observer_start` | `WH_MOUSE_LL` 与 Explorer WinEvent 观察器安装结果，以及进程完整性摘要 |
| `button_before/after` | 按钮注入前后坐标、目标窗口链、GUI 线程状态、`SendInput()` 插入数量和错误码 |
| `drag_first_before/after`、`drag_last` | 一段拖拽的首个和最后一个采样移动 |
| `hook_enter` | 调用 `CallNextHookEx()` 前观察到的低级鼠标事件 |
| `hook_exit` | 同一 `hookSequence` 的下游返回值和耗时 |
| `explorer_event` | Explorer 桌面 ListView 的焦点或选择状态变化 |
| `input_reset` | 会话或输入生命周期清理诊断状态 |

`hook_enter` 记录消息、坐标、`LLMHF_INJECTED` 和 `LLMHF_LOWER_IL_INJECTED`；`hook_exit` 使用相同 `hookSequence`，用于判断事件是否阻塞在下游钩子。观察器始终原样返回 `CallNextHookEx()` 的结果，不主动吞掉或重新注入事件。移动事件按 25 ms 节流，按钮事件不采样。

`button_after inserted=2 error=0` 只证明 `SendInput()` 接受了“绝对移动 + 按钮”两个 `INPUT`，不能证明 Explorer 或其他目标窗口最终处理了按钮事件。排查时必须把 `button_before/after`、对应 `hookSequence`、目标窗口状态和应用实际表现一起判断。

诊断会记录 HWND、PID、窗口类、进程文件名、坐标、完整性级别和 GUI 线程句柄状态；不记录窗口标题、用户名、桌面文件名、完整路径或画面内容。高频诊断本身可能改变钩子时序，结论应使用相同构建的启用/关闭 A/B 结果交叉验证。

## 接收渲染诊断

编码端 latency trace 的 `h264_encoder_init`、`h264_encode_end`、`h264_rate_target_changed` 将原 `bitrateKbps` 字段明确命名为 `requestedBitrateKbps`，表示请求目标而非实测输出；旧日志解析需兼容原字段。`bytes` 仅表示该次编码输出，不可单独当作码率。真实编码回归的 `measuredKbps` 另按媒体时间窗口累计计算。MF 记录目标变化不代表底层硬件已应用，不新增逐帧质量日志。

开启 session trace 后，`video_prepare` 按会话/路径变化及每 300 帧记录请求模式、实际格式、回退原因、`processUs` 和 `callbackToPreparedMs`（后者包含处理队列等待，不是纯转换耗时）；`video_present` 按首帧、会话/路径变化及每 300 帧记录请求/实际模式、generation、`uploadBytes`、`uploadSubmitUs`、`drawPresentCallUs`。后二者为 CPU 提交/调用耗时，不是 GPU 完成时间或屏幕发光时间。`video_renderer_fallback` 记录 I420 资源失败的 HRESULT 与会话 generation，不记录画面内容。

开启 latency trace 时强制 BGRA，并在延迟日志的 `video_renderer_selected` 中标记 `actual=bgra reason=latency_trace`，即使未开启 session trace 也能确认回退。GPU 正常路径的 `nConversionDoneAtMs` 保留未测量状态，不伪造 CPU 转换时间。隔离基准中的 `gpuUploadDraw*Us` 来自 GPU timestamp/disjoint 查询，延后四帧读取，未就绪/时钟失效样本和末尾未读样本单独计数。

## MF 编码诊断

`ffmpeg_build` 记录编码器与实际 FFmpeg 版本；旧后缀 `wrc-mf-patched-1` 表示历史实验 DLL，不能当作新版厂商依赖。
`mf_encoder_selected` 保留实际 MFT 名称。活动源码已取消 `mf_rate_update` 专属桥接，MF 不再修改运行时目标；
`h264_rate_update_unsupported` 每实例首次请求变化时记录请求与保留的初始值，明确不支持，不伪造已接受结果。
新依赖使用 `wrc-vendor-<模式>-<指纹>`，AMF 库日志的 `apiAcceptedTargetBps` 仅表示 API 接受；
实际输出必须用完整编码字节与媒体时间计算。运行目录切换和硬件验收状态见[厂商依赖构建](ffmpeg-vendor-build.md)。
新增显式 `--diagnostic-video-encoder=h264_qsv|h264_amf`，不静默回退；NVENC 尚未开放选择，`auto` 策略不变。
`h264_device_selected` 记录实际 D3D11 设备的名称、厂商/设备编号、驱动版本查询结果；
当前通过 CPU NV12 上传，不承诺与采集适配器相同。AMF 选 AMD、QSV 选 Intel，同厂商多卡使用
FFmpeg 首个可用设备，不在编码失败后偷偷换卡。
AMF 更新成功日志按进程最多每秒一条采样，错误不采样；采样缺失不能解释为没有执行重配置。
使用既有 latency trace，无需开启桌面输入 trace。
原始 FFmpeg 日志保留默认输出行为，未开启 trace 时不新增追踪文件。

## 隐私约束

- 键盘日志不记录虚拟键值、扫描码对应字符或 Unicode 正文。
- 剪贴板日志不记录正文、哈希、文件名或路径。
- 终端日志不记录命令、输出、工作目录或环境变量。
- 发现与最近设备日志不记录设备名称。
- TLS 与配对日志只记录验证方法、协议版本、密码套件、证书校验结果、短设备标识和失败原因；不记录私钥、TLS Exporter、六位配对码、完整证书、完整指纹或信令正文。
- 高频事件应采样或聚合，避免诊断本身明显增加延迟。
- Desktop input trace 不记录窗口标题、用户名、桌面文件名或进程完整路径；仅保留诊断所需的类名、文件名、句柄、PID 和坐标。
## H.264 level 诊断

沿用现有 latency trace，不增加命令行参数或独立日志文件：

- `h264_level_config`：`negotiatedLevel`、`encodingLevel`、`peakBudgetBps`，记录初始化时的约束。
- `h264_sps_validated`：首次或等级改变时记录 `actualLevel`；发送端同时记录协商/配置等级。
- `h264_level_rejected`：实际 SPS 的 profile/level/结构/尺寸不符合约束；不输出原始视频内容。profile/结构拒绝包含 `profileLevelId`、解析是否成功、`frameMbsOnly` 和 `maxRefFrames`，用于区分 profile 不匹配与结构解析失败，不放宽检查规则。

整数等级 31/42/52 分别代表 3.1/4.2/5.2。接收侧连续拒绝只记录首条，防止逐帧刷屏。
初始化和运行时超出等级/FPS 预算也通过 Qt warning 报错；不把配置值当成实际码流证据。
这些诊断不证明 HRD 合规、实际码率准确或双机实时性能。
# H.264 冻结诊断补充（2026-10-11）

仅开启 `--session-trace` 即可获得发送端 `encoder_watch` 和 `encoder_error`，不需要
`--trace` 或 `--latency-trace`，不启用水印、不改变接收端 GPU/BGRA 路径。

`encoder_watch` 由每个编码器诊断实例的独立线程约每秒输出，释放时另输出 `final=1`。
`instance` 标识当前诊断对象（只在其生命周期内唯一）；`entered/finished/outputs/errors/noOutput`
为累计计数，`submissions` 为进入 FFmpeg 编码封装的次数；两条记录的差值反映该时间段活动，不直接视为精准 FPS。
`outputs` 仅计输出回调成功；`errors` 为 Encode 返回失败次数；`noOutput` 是送帧成功但未出包次数。
`phase` 为 `initializing/idle/prepare/ffmpeg_encode/process_output/output_callback/closing`，
`phaseMs` 使用单调时钟表示停留时长。`idle` 可能是正常暂停或 WebRTC 未送帧，不能单独认定故障；
`ffmpeg_encode` 包含项目编码封装，不单凭此值断言驱动死锁。

每秒快照还包含最近 SetRates 的请求 bit/s、浮点帧率、初始化帧率、编码 level。
帧率反馈高于初始化值不再记录为致命错误，原始 requestedFps 仍可用于检查输入节奏波动。
码率越界、初始化/编码/输出回调/SPS 错误会记录原因；首次错误唤醒日志线程，
后续最多每秒一个错误事件，另保留最新错误和累计失败数。诊断队列只有一个待写错误槽。
所有该诊断的磁盘写入均在独立线程，快照锁不跨越编码、输出回调或磁盘写入。
关闭 trace 时不创建该线程；释放或重新初始化时回收，不修改原有错误标记、返回值或码率策略。
不记录图像、键盘内容；已有 session-trace 的其他隐私约束仍适用。
