# winRemoteControl

winRemoteControl 是一个面向 Windows 局域网的远程桌面原型。控制端可发现或手动连接被控端，在被控端审批后查看桌面，并发送鼠标、键盘和纯文本剪贴板内容。

项目优先保证生命周期清晰、失败可恢复和链路可诊断。目前不包含账号、公网中继、文件传输、音频、移动端或完整的多显示器控制。

## 当前能力

- IPv4 局域网发现、手动 IP 连接和最近设备。
- 被控端每次询问、自动接受或始终拒绝。
- DXGI 桌面采集、H.264 WebRTC 视频和 D3D11 渲染。
- `h264_mf` 优先，失败时自动回退 `libx264`。
- 鼠标、物理键盘、Unicode/IME 文本和双向纯文本剪贴板。
- 已审批会话中的独立 PowerShell 远程终端，具有单独权限确认。
- 能力协商、短暂 ICE 断线恢复、异步停止及初始化失败回滚。
- 会话日志、延迟日志和输入到画面反馈统计。

## 技术栈

- C++20、Qt 6、CMake、Visual Studio 2022
- WebRTC、FFmpeg、libyuv
- DXGI Desktop Duplication、D3D11、Win32 `SendInput`
- WebView2、React、Vite

React/WebView2 只负责展示与交互；会话、网络、采集、输入和安全判断均由 C++ 管理。

## 快速构建

前置条件与第三方目录说明见[构建与测试](docs/development/build-and-test.md)。在 Visual Studio 2022 x64 命令环境中执行：

```cmd
set QTDIR=D:\Qt\6.10.2\msvc2022_64
npm.cmd --prefix frontend install
npm.cmd --prefix frontend run build
cmake --preset release
cmake --build --preset release
ctest --preset release
```

生成的主程序通常位于 `build\release\Release\winRemoteControl.exe`。

运行时不加参数默认关闭诊断日志。需要完整日志时：

```cmd
build\release\Release\winRemoteControl.exe --trace
build\release\Release\winRemoteControl.exe --trace --log-dir "D:\wrc_logs"
```

默认 TCP 信令端口为 `39000`，局域网发现 UDP 端口为 `39001`。Windows 防火墙、校园网客户端隔离或虚拟机 NAT 可能阻止广播发现；此时仍可使用手动 IP 连接。

## 文档

- [架构概览](docs/architecture/overview.md)
- [会话生命周期](docs/architecture/session-lifecycle.md)
- [协议概览](docs/protocols/overview.md)
- [远程终端](docs/features/remote-terminal.md)
- [视频链路](docs/media/video-pipeline.md)
- [构建与测试](docs/development/build-and-test.md)
- [日志与诊断](docs/development/logging-and-diagnostics.md)
- [长期架构方针](PLAN.md)

## 安全边界

被控端必须显式开始监听，并按设置审批接入。活动会话保持可见且可主动停止。项目不提供隐藏运行、提权、绕过 Windows 安全提示或未经同意的采集与输入。
