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
Python 和 AI 直接连接测试。制作不含自动化能力的正式分发包时，显式配置
`WRC_BUILD_AUTOMATION_DRIVER=OFF`；该配置还会清理 Release 目录中遗留的驱动 DLL。

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

事件使用单调递增序号，并分为 Critical、State 和 Telemetry。帧统计主要从 state 读取；
`frame.progress` 最多每秒产生一条，Telemetry 最多保留 64 条，不会淹没配对、审批和错误
事件。创建 DriverSession 时会返回当时的 `eventCursor` 与 `sessionGeneration`，因此新会话
不会消费创建前的旧事件。每个控制事件都携带 `sessionGeneration`；若队列裁剪造成序号
缺口，响应会设置 `hasGap=true`，Python 立即抛出 `EventHistoryLost`。

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
session.trigger_command("session.disconnect", idempotency_key="disconnect-once")
session.quit()
```

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

隐私遮罩及可能影响工作站状态的测试还要求显式设置环境变量
`WRC_RUN_DESTRUCTIVE_AUTOMATION=1`。普通 CI 工作流绝不会启用这些测试。

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
- 正式对外分发包应使用 `WRC_BUILD_AUTOMATION_DRIVER=OFF`；开发 Release 目录保留 DLL。
