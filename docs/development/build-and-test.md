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

注意核对实际启动目录：普通 Release 为 `build/Release`，生产 Release 为 `build/production/Release`，二者的 `frontend` 资源互相独立。前端由应用的 POST_BUILD 步骤部署；仅执行 Vite 构建或只更新另一个 Release 目录，不会更新当前程序的页面。交付 UI 修复时，应检查目标目录 `frontend/index.html` 引用的资源文件及内容与 `frontend/dist` 一致，并重启窗口加载新页面。

### 设置页视觉与交互回归

设置分类导航保留浅色卡片外框（细边框、8px 圆角），不恢复装饰编号和重阴影。`daylight` 浅色主题统一采用中性灰白背景 `#f3f3f3`、卡片 `#fbfbfb` 与中性灰边框/正文，减少原来的蓝灰色倾向；此配色作用于使用该主题变量的所有页面，其他主题不变。此处为静态配色，不模拟 Windows 随壁纸变化的 Mica 材质。

设置页使用独立的 `settings-windows-style` 作用域：保留五个设置分类与原有命令、确认逻辑，采用系统无衬线字体、轻边框分组和连续审批设置行，去除英文编号、装饰圆环及重阴影。窗口宽度不超过 900 CSS 像素时，分类导航换行到内容上方，避免双侧栏挤压设置项。其他功能页面不启用此作用域。

已通过 18 组 Edge 无头浏览器组合（1400×900、1000×800、640×600；DPI 1/1.5；daylight/midnight/nordic-mist），覆盖全部分类的宽度边界、安全确认与 Escape、禁用状态、倒计时/端口/快捷键模拟命令、主题键盘选择及页面作用域。测试使用模拟 Bridge，不修改真实权限、设备或设置；原生 WebView2 复验仍待完成。

### 最近设备页面与菜单回归

“我的设备”及未连接的设备详情页使用 `devices-windows-style` 作用域：系统无衬线字体、28px 半粗标题、平实主题背景和约 6px 控件圆角；设备仍在侧栏，选择后仍在原主区域显示详情。远程协助、设置和已连接会话页面不启用该样式，原有主题选择与默认主题不变。

视觉改版已通过 12 组 Edge 无头浏览器组合（1100×800、800×600、640×480；DPI 1/1.5；daylight/midnight），覆盖空列表、最近/在线设备、长名称、连接中、错误显示、键盘焦点、导航作用域及模拟连接命令；菜单另通过下述 18 组回归。已检查改版前后明暗截图。原生 WebView2 复验仍待完成。已知原有问题：小窗口下底部错误浮层可能覆盖连接按钮，本轮保持布局与错误处理逻辑，不将其记为已修复。

“移除最近记录”菜单挂在 dashboard 根容器，与侧栏和主内容并列，避免被主内容的层叠上下文限制；固定定位坐标至少保留 8 CSS 像素的左、上边距。
修改相关 UI 后，在侧栏右键最近设备，检查跨侧栏边界的菜单完整显示且按钮全区域可点击；覆盖窗口边缘、小窗口、主题和缩放变化，并确认 Escape、点击外部和失焦关闭正常，移除命令只作用于目标记录且不触发连接。浏览器验证不能替代最终 WebView2 桌面程序复验。

本次修复已通过 Edge 无头浏览器的 18 组组合（1100×800、640×480、400×360；DPI 1/1.5；daylight、midnight、nordic-mist）。使用虚拟设备和模拟 Bridge 验证按钮九点命中、边缘定位、三种关闭方式及唯一目标移除命令；恢复旧 DOM 层级可复现遮挡。原生 WebView2 窗口复验仍待完成。

## Release 构建

日常开发默认只构建和更新 `build/Release`（`release` preset），包括前端资源部署；只有明确要求生产构建或打包时才更新 `build/production/Release`，不为同步两份运行目录而重复构建。

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
4. 验证鼠标单击、双击、桌面框选、窗口拖动、滚轮和多按钮状态；确认双击第二组按钮保持同一远端坐标。
5. 验证键盘捕获后 Win、Alt+Tab 等普通按键只作用远端；`Ctrl+Alt+Enter` 释放捕获，单击画面可重新捕获。
6. 验证用户“仅控制端响应”快捷键只作用本机，`Ctrl+Alt+F` 切换全屏，Ctrl/Shift+鼠标操作会先刷新修饰键。
7. 验证左右修饰键、自动重复、IME/Emoji 和剪贴板双向同步。
8. 验证短暂断网恢复、超过 10 秒失败、重新审批和重复断开；重连期间不得遗留本地钩子或远端按下状态。
9. 推流中关闭窗口，确认 UI 不冻结且没有运行中 `QThread` 被析构。
10. 占用 TCP 端口触发失败，释放或更换端口后再次监听，确认 WebRTC 初始化资源可干净重建。

广播发现可能被校园网客户端隔离、Windows 防火墙或虚拟机 NAT 阻止。手动 IP 成功但发现失败通常是网络广播策略问题，不代表 TCP/WebRTC 链路故障。
