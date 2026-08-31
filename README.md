# winRemoteControl

面向 Windows 局域网的远程桌面客户端。

[![Core tests](https://github.com/lyn-un/winRemoteControl/actions/workflows/core-tests.yml/badge.svg)](https://github.com/lyn-un/winRemoteControl/actions/workflows/core-tests.yml)
![Windows 10 和 Windows 11](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D4)
![Qt 6](https://img.shields.io/badge/Qt-6-41CD52)

[主要功能](#主要功能) · [快速开始](#快速开始) · [构建](#构建) · [项目文档](#项目文档) · [安全说明](#安全说明)

> **开发状态：** 项目仍处于开发阶段，目前专注于同一局域网内的 Windows 到 Windows 远程控制。它尚未提供公网中继、账号系统、音频转发、后台服务和面向普通用户的安装程序。

winRemoteControl 允许两台 Windows 电脑在局域网内建立经过认证的点对点会话。控制端可以查看和操作被控端桌面，并按授权使用剪贴板、远程终端和双向文件传输。被控端始终显示会话状态，并可随时停止连接。

核心会话、安全检查、采集、编码、传输和输入处理均在 C++ 中完成；Qt 管理原生窗口与生命周期，WebView2 和 React 只负责界面展示与交互。

## 主要功能

| 类别 | 能力 |
| --- | --- |
| 连接 | IPv4 局域网发现、手动 IP、最近设备、可信设备、短暂断线恢复 |
| 身份与权限 | Schannel TLS 1.2/1.3、设备证书、六位配对码、按功能划分的可信权限 |
| 远程桌面 | DXGI Desktop Duplication、H.264 WebRTC、D3D11 渲染、显示器切换 |
| 画质 | 画质与帧率独立设置；按显示器和双方能力支持 30/60/90/120/144 FPS |
| 远程输入 | 鼠标、物理键盘、Unicode/IME 文本输入和常用系统组合键 |
| 会话工具 | 双向纯文本剪贴板、受控 PowerShell 终端、双栏双向文件传输 |
| 安全控制 | 隐私屏、断开后锁屏、按认证设备保存偏好、物理紧急恢复快捷键 |
| 可维护性 | 异步停止、失败回滚、资源生命周期追踪、浅色/深色主题、自动化 Driver |

文件传输支持普通文件和文件夹、暂停恢复、冲突处理、SHA-256 完整性校验及临时文件原子提交。远程终端、文件传输、输入和隐私控制都有独立权限，不会因设备已受信任而自动获得后来新增的能力。

## 当前范围

| 项目 | 当前状态 |
| --- | --- |
| Windows 控制 Windows | 开发中，可进行双机验证 |
| Android 客户端 | 在独立的 `winRemoteControl-android` 仓库中开发，不包含在本仓库 |
| macOS / Linux | 尚未实现 |
| 公网中继 / NAT 穿透 | 尚未实现 |
| 音频、账号、云服务 | 尚未实现 |

## 快速开始

当前版本需要从源码构建。准备两台位于同一局域网的 Windows 10/11 x64 电脑，并在两端运行同一版本的程序：

1. 在被控端开始监听，按需允许 Windows 防火墙放行程序。
2. 在控制端通过局域网发现选择设备；发现不可用时可手动输入 IPv4 地址。
3. 首次连接时，在两端核对六位配对码，并由被控端确认连接权限。
4. 进入远程桌面后，再按已授予的权限使用输入、剪贴板、终端或文件传输。
5. 任意一端都可主动断开；被控端在会话期间始终提供可见的停止入口。

默认 TCP 信令端口为 `39000`，局域网发现 UDP 端口为 `39001`。校园网客户端隔离、虚拟机 NAT 或防火墙可能阻止广播发现，这种情况下手动 IP 连接仍可能正常工作。

## 构建

### 环境要求

- Windows 10/11 x64。
- Visual Studio 2022，安装 MSVC x64 和 Windows SDK。
- CMake 3.28 或更高版本。
- Qt 6 MSVC 2022 x64，至少包含 Core、Gui、Widgets 和 Network。
- Node.js 与 npm。
- 已准备项目所需的 WebRTC、FFmpeg 和 WebView2 依赖目录。

第三方依赖的目录结构和测试预设见[构建与测试](docs/development/build-and-test.md)。

### 开发 Release

在 Visual Studio 2022 x64 命令环境中执行：

```cmd
set QTDIR=D:\Qt\6.10.2\msvc2022_64
npm.cmd --prefix frontend install
npm.cmd --prefix frontend run build
cmake --preset release
cmake --build --preset release
ctest --preset release
```

主要产物：

- 程序：`build\Release\winRemoteControl.exe`
- 自动化 Driver：`build\Release\automation\wrcdriver.dll`
- CMake 构建目录：`build\cmake-release`

### 正式产物

`production-release` 使用独立目录，并检查产物中没有混入自动化 Driver、测试程序或资源探针：

```cmd
cmake --preset production-release
cmake --build --preset production-release
```

产物位于 `build\production\Release`。

### 日志与诊断

程序默认关闭详细诊断日志。排查问题时可显式启用：

```cmd
build\Release\winRemoteControl.exe --trace
build\Release\winRemoteControl.exe --trace --log-dir "D:\wrc_logs"
```

## 架构概览

| 层 | 职责 |
| --- | --- |
| C++ Core | 会话状态、权限、认证、采集、编解码、WebRTC、输入和文件系统操作 |
| Qt 6 | 应用生命周期、原生窗口、线程协调和 C++/WebView2 Bridge |
| WebView2 + React | Dashboard、远程桌面菜单、终端与文件传输界面 |
| Windows Adapter | DXGI、D3D11、Schannel、SendInput 及其他 Win32 集成 |

模块遵循显式所有权和端口/适配器边界，前端不保存安全敏感状态，也不直接接触文件正文或远程传输数据。

## 项目文档

### 架构与协议

- [架构概览](docs/architecture/overview.md)
- [会话生命周期](docs/architecture/session-lifecycle.md)
- [协议概览](docs/protocols/overview.md)
- [设备身份、mTLS 配对与权限](docs/security/device-identity.md)
- [长期架构方针](PLAN.md)

### 功能

- [视频链路](docs/media/video-pipeline.md)
- [帧率设置与高帧率方案](docs/plans/frame-rate-settings.md)
- [远程终端](docs/features/remote-terminal.md)
- [双向文件传输](docs/features/file-transfer.md)
- [远程会话安全控制](docs/features/session-security-controls.md)

### 开发与测试

- [构建与测试](docs/development/build-and-test.md)
- [命令级自动化 Driver](docs/automation-driver.md)
- [隐私控制平台验证](docs/development/privacy-platform-validation.md)
- [日志与诊断](docs/development/logging-and-diagnostics.md)

## 测试

提交修改前至少运行：

```cmd
npm.cmd --prefix frontend run build
cmake --build --preset release
ctest --preset release
git diff --check
```

常规自动化测试不会启动真实隐私屏、关闭显示器、锁定工作站或向真实目录传输文件。跨进程推流和隐私控制测试需要使用文档中规定的环境变量显式启用；双机画面、输入和系统行为仍需在受控环境中手动验证。

## 安全说明

> **安全警告：** 远程控制软件只能用于你拥有或已明确获准控制的设备。请勿将本项目用于未经授权的访问、监控或数据获取。

- 被控端必须主动开始监听，并按当前策略接受连接。
- 首次配对需要两端核对由当前 TLS 会话生成的六位数字。
- 活动会话保持可见，被控端可以随时停止连接。
- 所有敏感功能都检查当前角色、会话代次、协商能力和最终权限。
- 项目不提供隐藏运行、提权、绕过 Windows 安全提示或未经同意的采集与输入。

更完整的信任模型和权限边界见[设备身份、mTLS 配对与权限](docs/security/device-identity.md)。

## 参与开发

提交 Issue 时，请附上系统版本、复现步骤、期望行为以及去除设备信息后的相关日志。提交代码前，请保持 C++ 核心与 React 展示层的边界，并完成与改动范围相称的构建和测试。

项目尚未提供独立的贡献指南；构建流程、测试矩阵和架构约束以本 README、`docs/` 和 `AGENTS.md` 为准。
