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

普通生产构建默认关闭可选驱动：

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

Release DLL 输出到 `build/Release/automation/wrcdriver.dll`。从发布包中删除该目录即可
禁用自动化。使用 `WRC_BUILD_AUTOMATION_DRIVER=OFF` 的普通应用构建还会在链接后删除
残留的 `automation/wrcdriver.dll`，避免早期开发构建产生的驱动意外进入正式 Release
目录。

开发阶段如果需要在 Release 构建目录中保留驱动，可以显式配置：

```powershell
cmake --preset release -DWRC_BUILD_AUTOMATION_DRIVER=ON
cmake --build --preset release --target wrcdriver
```

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

请求正文上限为 16 KB。应用命令的 Host 超时时间为 5 秒。事件使用单调递增序号，队列
最多保留 512 项。命令进入 Host Bridge 时会记录远程会话 generation；如果命令在 GUI
线程执行前已经切换到另一个远程会话，就会以 `stale_generation` 拒绝。HTTP 服务的每个
TCP 连接只接受一个请求，并始终返回 `Connection: close`。因此单连接请求上限为 1，
畸形输入会在首次失败时关闭，不会保留连接并反复解析。

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
session.trigger_command("session.disconnect")
session.quit()
```

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
- 正式发布的 Release 包不得包含 `automation/wrcdriver.dll`。
