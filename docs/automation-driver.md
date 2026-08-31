# 命令级自动化驱动

winRemoteControl 提供一个可选的、仅用于开发的自动化驱动，其设计借鉴了 WPSDriver
的命令架构。驱动通过稳定的命令注册表控制现有 C++ 应用服务；它不会检查 QWidget
或 WebView2 元素，也不会模拟鼠标或键盘输入。

## 组成部分

```text
Python wrcdriver SDK
  -> 本机回环 HTTP/JSON
  -> automation/wrcdriver.dll
  -> KDriverSession 和请求路由
  -> 带版本的 C Host API
  -> KAutomationHostBridge
  -> KApplicationCommandRegistry
  -> 现有 ViewModel、PreferenceService 和 SessionCoordinator
```

`KDriverSession` 是 Python 与本机进程之间的控制会话。它独立于
`KSessionCoordinator` 管理的远程控制会话。

主程序不会链接 `wrcdriver.lib`。启动时只检查
`<applicationDir>/automation/wrcdriver.dll`。如果文件不存在，就不会创建自动化端点或
发现文件。DLL 存在时，其 Host ABI 版本、体系结构、Debug/Release 运行时、Qt 主版本和
集中定义的自动化构建 ID 必须全部与主程序匹配。构建 ID 只在 Host API 契约中定义一次，
避免主程序、DLL 和加载器测试悄然使用不同的常量。

## 构建

项目当前处于开发阶段，`dev` 和 `release` 预设都会构建可选驱动：

```powershell
cmake --preset release
cmake --build --preset release
```

使用以下命令构建驱动及其轻量依赖测试：

```powershell
cmake --preset automation-tests
cmake --build --preset automation-tests
ctest --preset automation-tests
```

Release DLL 输出到 `build/Release/automation/wrcdriver.dll`。当前开发目录保留该文件，便于
Python 和 AI 直接连接测试。`automation-tests` 使用自己的隔离运行时目录，不会覆盖开发
Release。制作不含自动化能力的正式分发包使用独立预设：

```powershell
cmake --preset production-release
cmake --build --preset production-release
```

正式运行时输出到 `build/production/Release`。该预设关闭 Driver、测试和资源探针，并在
构建结束时检查产物；如果发现 `automation/wrcdriver.dll`、Python SDK、测试程序或资源
探针，构建会直接失败。正式构建不会删除或修改 `build/Release` 中的开发 Driver。

## 本机传输

驱动监听操作系统自动分配的端口，并且只绑定 `127.0.0.1`。每个 HTTP 请求都必须在
`X-WRC-Token` 中携带对应进程的随机令牌。发现元数据会原子写入
`%LOCALAPPDATA%/winRemoteControl/automation/<pid>.json`，并在正常退出时删除。令牌和
命令参数不会写入驱动日志。

固定路由如下：

```text
GET    /status
POST   /session
DELETE /session/:sessionId
POST   /session/:sessionId/command/trigger
GET    /session/:sessionId/state
GET    /session/:sessionId/events?sinceSequence=N
```

请求正文上限为 16 KB。发现文件存在只表示端点已经创建；`/status.ready` 只有在 Driver
和业务 Host 都完成初始化后才为 `true`，Python 的 `attach()` 与 `launch()` 会等待该状态。
SDK 还会核对发现文件中的 PID、进程启动时间、HTTP 协议版本和固定 Build ID；旧 Driver
或其他进程遗留的发现文件不会被复用。

命令提交给 Host 时携带相对 `timeoutMs`。HostBridge 在任务入队前用单调时钟生成截止点，
并在真正调用命令注册表之前再次检查。`command_timeout` 因而保证命令没有开始执行，之后
也不会产生副作用；若命令已经进入业务执行但响应窗口耗尽，则返回
`command_execution_started`、`retryable=false`、`outcomeUnknown=true`，调用方不得自动
重试。需要安全重试的调用可传入 `idempotencyKey`，相同键、命令和参数只执行一次。
幂等键在当前 Driver 进程内全局有效，不依赖单个 DriverSession；记录使用单调时钟保留
30 分钟，容量为1024条。HTTP 等待结束后，已经开始的 Host 命令仍会继续被跟踪；完成
回调到达后，相同 key 会从 `command_execution_started` 收敛为真实最终结果。相同 key
用于不同命令或参数时返回 `idempotency_key_conflict`。

事件使用单调递增序号，并分为 Critical、State 和 Telemetry。帧统计主要从 state 读取；
`frame.progress` 最多每秒产生一条，Telemetry 最多保留 64 条，不会淹没配对、审批和错误
事件。创建 DriverSession 时会返回当时的 `eventCursor` 与 `sessionGeneration`，因此新会话
不会消费创建前的旧事件。每个控制事件都携带 `sessionGeneration`；若队列裁剪造成序号
缺口，响应会设置 `hasGap=true`，Python 立即抛出 `EventHistoryLost`。

Python 中 `poll_events()` 从当前 Session 游标消费新事件并自动推进；`get_events_since(N)`
用于无副作用的历史查询。兼容入口 `get_events()` 等同 `poll_events()`，显式传入整数时
等同历史查询。发生 gap 时游标保持不变，调用方读取完整 state 后可通过
`reset_event_cursor()` 明确选择恢复位置。

命令进入 Host Bridge 时会记录远程会话 generation；如果命令在 GUI 线程执行前已经切换
到另一个远程会话，就会以 `stale_generation` 拒绝。HTTP 服务的每个 TCP 连接只接受一个
请求，并始终返回 `Connection: close`。因此单连接请求上限为 1，畸形输入会在首次失败时
关闭，不会保留连接并反复解析。

state 中的 `currentError` 表示当前尚未恢复的结构化故障，恢复到 Connected 或 Streaming
后会清空；`lastError` 保留最近一次故障供诊断，不能用来判断当前连接是否健康。两者均
包含 code、domain、stage、retryable、technicalMessage、时间和 generation。

端到端推流测试会在 `stream.start` 前记录帧基线，随后要求至少两个不同采样点的帧计数
和时间戳继续增长；单帧和旧累计计数均不能判定成功。`stream.stop` 后还会等待状态回到
Connected，并确认帧计数和时间戳经过安静窗口后不再增长。

## Python SDK

以可编辑模式安装软件包：

```powershell
pip install -e automation/python
```

示例：

```python
from wrcdriver import WrcApplication

application = WrcApplication.attach(pid=12345)
session = application.create_session()
print(session.get_state())
print(session.get_supported_commands())
session.trigger_command("session.disconnect", idempotency_key="disconnect-once")
session.quit()
```

### 可用业务命令

`get_supported_commands()` 返回当前构建实际注册的命令 ID。AI 不需要识别网页按钮或坐标，
而是调用与按钮相同的 C++ 业务服务。当前命令按功能分组如下：

| 功能 | 命令 ID | 主要参数 |
| --- | --- | --- |
| 角色与连接 | `application.set_role` | `role: controller/controlled` |
| 启动监听 | `signaling.start_server` | `port` |
| 直接连接 | `session.connect` | `host`, `port` |
| 重试/断开 | `session.retry`, `session.disconnect` | 无 |
| Access/配对审批 | `access.respond`, `pairing.respond` | `requestId`, `accepted`，配对接受时还需 `permissions` |
| 推流 | `stream.start`, `stream.stop` | 无 |
| 本地预览 | `capture.preview.start`, `capture.preview.stop` | 无 |
| 画质 | `desktop.quality.set` | `fps`, `width`, `height`, `bitrateKbps` |
| 局域网发现 | `discovery.refresh`, `discovery.connect` | 连接时传 `deviceId` |
| 最近设备 | `recent.list`, `recent.connect`, `recent.remove`, `recent.terminal.open` | 后三者传 `deviceId` |
| 设置 | `settings.get`, `settings.update`, `settings.theme.set` | 设置字段或 `themeId` |
| 可信设备 | `trusted.list`, `trusted.update`, `trusted.revoke`, `trusted.repair` | `deviceId`，更新时传 `alias`, `permissions` |
| 剪贴板 | `clipboard.get`, `clipboard.set_enabled` | `enabled` |
| 终端 | `terminal.open`, `terminal.respond`, `terminal.input`, `terminal.resize`, `terminal.close`, `terminal.get` | 尺寸、审批信息或 Base64 输入 |
| 文件传输会话 | `file_transfer.open`, `file_transfer.stop`, `file_transfer.get` | 无 |
| 文件浏览 | `file_transfer.roots`, `file_transfer.navigate`, `file_transfer.navigate_path`, `file_transfer.up`, `file_transfer.refresh` | `pane` 及 state 返回的不透明 listing/entry ID |
| 文件任务 | `file_transfer.copy`, `file_transfer.pause`, `file_transfer.resume`, `file_transfer.cancel`, `file_transfer.retry`, `file_transfer.resolve_conflict`, `file_transfer.clear_completed` | state/event 返回的不透明 ID |
| 安全偏好 | `security.privacy.set`, `security.post_session.set` | `mode` 或 `action` |
| 窗口 | `window.main.*`, `window.desktop.*`, `window.file_transfer.*` | 最小化、切换最大化或关闭 |

完整 state 还包含 `lanDevices`、`recentDevices`、`trustedDevices`、
`applicationSettings`、`clipboard`、`terminal`、`fileTransfer`、左右文件栏快照、任务和冲突。
目录复制只能提交 state/event 返回的 `listingId`、`entryId` 和 `taskId`，不能用 Python
直接向核心传最终路径或文件内容。终端输出通过 `terminal.output` 事件返回 Base64 数据，
输入可直接使用 `session.send_terminal_input("命令文本")`。

控制中心菜单、设置页签等纯导航元素没有独立命令；对应业务功能已经由上述语义命令覆盖。
驱动也不提供任意桌面坐标点击或任意 Shell 路由，避免绕过现有权限与会话状态检查。

SDK 不会根据异常名称盲目重试。所有驱动异常都保存 `error_code`、`driver_status`、
`request_id`、`retryable` 和 `outcome_unknown`；另外提供 `ApplicationNotReady`、
`StaleSessionGeneration`、`ApplicationShuttingDown` 与 `EventHistoryLost` 等稳定类型。
只有 `retryable=True` 且 `outcome_unknown=False` 时，调用方才可以结合幂等键决定是否重试。

命令行工具将机器可读的 JSON 写入标准输出，将诊断信息写入标准错误：

```powershell
python -m wrcdriver --pid 12345 status
python -m wrcdriver --pid 12345 state
python -m wrcdriver --pid 12345 trigger session.disconnect
```

`WrcApplication.launch()` 始终传入隔离的 `--data-dir`，并将设备身份标记为自动化测试
身份。测试进程正常退出时会删除其临时 CNG 密钥和证书，普通应用配置中的设备身份仍会
持久保存。

## 双进程测试

连接、推流和断开测试需要交互式 Windows 桌面及真实 DXGI 采集，因此必须显式启用：

```powershell
$env:WRC_RUN_E2E = '1'
$env:WRC_EXECUTABLE = (Resolve-Path 'build/Release/winRemoteControl.exe')
$env:PYTHONPATH = (Resolve-Path 'automation/python')
python -m unittest discover -s automation/tests -p 'test_*.py' -v
```

编排器只有在两端的 TLS Exporter 数字校验码一致后才确认配对，随后使用正常的配对和
Access 审批命令，不会绕过身份认证或权限检查。

隐私遮罩测试还要求显式设置 `WRC_RUN_PRIVACY_E2E=1`；真实锁屏等测试要求更严格的
`WRC_RUN_DESTRUCTIVE_AUTOMATION=1`。普通 CI 工作流绝不会启用这两类测试。跨进程隐私
测试会复用隔离 profile 验证设备身份和偏好，再在最终测试进程退出时删除临时 CNG 密钥。

人工验收时可从仓库根目录运行统一脚本。默认会先复制一份不含 Driver 的临时 Release，
确认主程序存活且没有发现文件，再运行连接、审批、推流和断开的双进程测试；临时副本、
隔离 profile 和测试进程会在结束时清理：

```powershell
.\automation\tests\run_acceptance.ps1
```

只有准备好测试真实隐私遮罩时才增加 `-IncludeDestructive`。该开关仍不会测试
`displayOff`，而锁屏动作会在断开前恢复为 `none`；若测试中途异常，仍应先确认设置已经
恢复，再关闭测试窗口。

```powershell
.\automation\tests\run_acceptance.ps1 -IncludeDestructive
```

GitHub 托管的 Windows Runner 无法为 DXGI 采集提供可靠的交互式桌面。因此，工作流在
托管 Runner 上执行命令、传输和加载器测试，并为具有交互式桌面的自托管 Windows
Runner 定义 `automation-e2e` 任务。只有仓库变量 `WRC_ENABLE_AUTOMATION_E2E` 设置为
`1` 时才启用该任务；除非在自托管 Runner 上另行明确授权，否则隐私屏和工作站锁定
测试仍保持关闭。

## 安全边界

- 只能执行在 `KApplicationCommandRegistry` 中注册的命令 ID。
- 所有路由都不能调用任意 QObject 槽、JavaScript、Shell 或动态库路径。
- 自动化接口不会通过信令或 WebRTC DataChannel 暴露给远端。
- 远程配对、Access 审批和权限强制逻辑保持不变。
- 正式对外分发包必须使用 `production-release`；开发 Release 目录保留 DLL。
