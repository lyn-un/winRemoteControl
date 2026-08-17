# 构建与测试

## 前置条件

- Windows 10/11 x64。
- Visual Studio 2022，安装 MSVC x64 与 Windows SDK。
- CMake 3.28 或更高版本。
- Qt 6 MSVC 2022 x64，至少包含 Core、Gui、Widgets、Network。
- Node.js 与 npm，用于构建 React/Vite 前端。
- 项目内已有以下依赖目录：
  - `third_party/webrtc/src`，且 `out/wrc_release_x64_msvcstl/obj/webrtc.lib` 已构建。
  - `third_party/ffmpeg`，包含 headers、libraries 和运行时 DLL。
  - `third_party/webview2`，包含 native x64 SDK。

完整桌面程序需要 WebRTC、FFmpeg 和 WebView2；`core-tests` preset 可以在不启用这三项 adapter 的情况下构建核心测试。

## 构建前端

首次拉取或依赖变化后：

```cmd
npm.cmd --prefix frontend install
```

每次修改 React 后：

```cmd
npm.cmd --prefix frontend run build
```

产物写入 `frontend/dist`，桌面程序运行时加载该 UI。

## Release 构建

在 Visual Studio 2022 x64 命令环境中设置 Qt：

```cmd
set QTDIR=D:\Qt\6.10.2\msvc2022_64
cmake --preset release
cmake --build --preset release
ctest --preset release
```

主程序位于 `build\Release\winRemoteControl.exe`，该目录只保留可部署程序、DLL、Qt 插件和前端资源。静态库输出到 `build\lib\Release`，单元测试输出到 `build\tests\Release`，性能测试输出到 `build\benchmarks\Release`。

## 轻量核心测试

不需要构建完整 WebRTC/FFmpeg/WebView2 adapter 时：

```cmd
set QTDIR=D:\Qt\6.10.2\msvc2022_64
cmake --preset core-tests
cmake --build --preset core-tests
ctest --preset core-tests
```

## Schannel 安全链路测试

安全 preset 只构建协议、设备身份、配对事务和真实 Schannel TCP 测试，不依赖 WebRTC、FFmpeg 或 WebView2：

```cmd
set QTDIR=D:\Qt\6.10.2\msvc2022_64
cmake --preset security-tests
cmake --build --preset security-tests
ctest --preset security-tests
```

GitHub Actions 的 `security-tests` job 会在 `windows-2022` 上执行真实客户端/服务端 mTLS 握手、对端证书提取、TLS Exporter、加密信令收发及未认证输入上限测试；每个测试有 30 秒超时。

## 测试结构

- Codec：边界、畸形字段、版本、大小限制和往返编码。
- 状态机与协调器：审批、能力协商、断线恢复、异步停止、超时和 generation 隔离。
- adapter/component：TCP、发现、持久化、Capture、WebRTC 初始化回滚和线程生命周期。
- 前端：Vite production build，保证 bridge 使用的页面可生成。

提交前至少执行：

```cmd
npm.cmd --prefix frontend run build
cmake --build --preset release
ctest --preset release
git diff --check
```

## 双机手动检查

1. 被控端开始监听，控制端通过发现或 IP 连接。
2. 验证询问、接受、拒绝、取消和超时。
3. 验证静态首帧、持续画面、画质切换和 H.264 软件回退环境。
4. 验证鼠标最终位置、左右修饰键、IME/Emoji 和剪贴板双向同步。
5. 验证短暂断网恢复、超过 10 秒失败、重新审批和重复断开。
6. 推流中关闭窗口，确认 UI 不冻结且没有运行中 `QThread` 被析构。
7. 占用 TCP 端口触发失败，释放或更换端口后再次监听，确认 WebRTC 初始化资源可干净重建。

广播发现可能被校园网客户端隔离、Windows 防火墙或虚拟机 NAT 阻止。手动 IP 成功但发现失败通常是网络广播策略问题，不代表 TCP/WebRTC 链路故障。
