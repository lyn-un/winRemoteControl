# 远程 PowerShell 终端

远程终端是已审批 WebRTC 会话的可选附加能力。它不会绕过主连接审批；每次打开终端时，都需要被控端单独确认。

## 整体链路

```mermaid
flowchart LR
    WT["Windows Terminal (wt.exe)"] --> Relay["wrcTerminalRelay.exe"]
    Relay -->|"当前用户命名管道"| Controller["控制端 KTerminalSessionService"]
    Controller -->|"framed reliable terminal DataChannel"| Controlled["被控端 KTerminalSessionService"]
    Controlled --> ConPTY["Windows ConPTY"]
    ConPTY --> PS["PowerShell 7 / Windows PowerShell 5.1"]
```

Windows Terminal 只是控制端的原生终端界面。真正的 PowerShell 进程始终运行在被控端，由 `KWindowsPseudoConsole` 通过 ConPTY 托管。

## 组件

- `KTerminalSessionService`：管理终端审批、generation、单实例、断网暂停和同时受消息数、总字节数约束的输入/输出队列。
- `KTerminalCommandDispatcher`：为有副作用的 Open、Accepted、Resize、Close、Exited 和 Error 命令提供 command ID、ACK、有限重试、超时和幂等结果缓存；待 ACK 表和结果缓存同时限制条目数及编码字节数。
- `KWindowsTerminalFrontend`：检测 `wt.exe` 和 Relay，创建随机命名管道与一次性 token，启动或聚焦专用 Windows Terminal 窗口。
- `wrcTerminalRelay.exe`：无 Qt 的本地字节中继，把 Windows Terminal 的 stdin/stdout 与主程序命名管道相连，并上报终端尺寸。
- `KWindowsPseudoConsole`：在被控端以当前用户权限优先启动 PowerShell 7，失败时回退 Windows PowerShell 5.1，并通过 Job Object 清理进程树。

## 本地 IPC 边界

- `QLocalServer` 只允许当前用户访问，每次打开使用随机管道名和 128-bit 一次性 token。
- 只允许一个 Relay 客户端；首帧必须是 `Hello`，验证失败立即断开。
- 本地帧类型为 `Hello / Input / Output / Resize / Close`，单帧最大 64 KiB，解码器支持拆包和粘包。
- 帧头和 Resize payload 显式使用小端序，累计接收缓冲、待输出队列和 `QLocalSocket` 写缓冲均有上限及高低水位。
- 主程序启动 Relay 前会持有 `wrcTerminalRelay.exe` 的只读文件句柄，并禁止写入和删除共享直到终端关闭，避免校验与启动之间被替换；正式安装目录的 ACL 仍应由安装程序配置。
- Relay 不连接网络、不解析命令，不记录 token、输入或输出内容。

管道名和一次性 token 会作为 Relay 启动参数，因此同一登录用户下、能够读取其他进程命令行的恶意进程不在当前威胁模型内。这里的目标是阻止其他 Windows 用户和未持有 token 的进程接入，并在启动到终端关闭期间锁定 Relay 可执行文件；未来安装器仍需把程序放入普通用户不可写的目录并配置 ACL。

## 权限和生命周期

内部状态由 `KTerminalStateMachine` 约束为 `Closed -> AwaitingApproval -> Opening -> Running -> Closing/Failed`。每次打开都会产生新的终端 `requestId` 并重新审批；完整重连产生新 generation 后同样必须重新审批。

每个 terminal 二进制消息都带固定格式头：`magic + version + direction + requestId + sequence + payloadLength`。接收端只接受当前终端实例，拒绝旧 requestId、重复或倒序 sequence。DataChannel 背压时输入进入 256 KiB/256 条的有界队列，同时暂停 Relay 的输入读取并在 low-watermark 后恢复；超过上限会明确关闭本次终端，不会在命令中间静默丢字。

`Reconnecting` 期间保留 Windows Terminal 窗口和远端 PowerShell，但丢弃新输入。恢复失败、Terminal DataChannel 关闭或主会话结束时，命名管道会关闭，被控端 Job Object 终止 PowerShell 及子进程。用户关闭 Windows Terminal 只会停止终端，不会断开远程桌面。

## 能力条件

- 控制端：必须能找到 `wt.exe`，且 `wrcTerminalRelay.exe` 与主程序同目录。
- 被控端：只需 Windows 10 1809 或更高版本的 ConPTY，不需要安装 Windows Terminal。
- 任一端不满足本地条件时，不声明 `terminal` 能力，不影响视频、键鼠和剪贴板。
- 不提权、不触发 UAC、不修改 Windows Terminal 配置，不启用 SSH Server 或改动防火墙。
