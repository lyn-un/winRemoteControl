# winRemoteControl 后续开发与架构方针

> 本文是长期约束，不是一次性任务清单。当前实现事实与操作方法以 [README](README.md) 和 [`docs`](docs/architecture/overview.md) 为准；架构边界发生变化时，应先更新设计再修改实现。

## 1. 阶段目标

当前阶段只建设 Windows 主机之间的局域网远程控制：连接、审批、画面、键鼠、文本剪贴板、断线恢复和可靠停止必须清晰可诊断。

暂不引入公网中继、账号体系、文件传输、音频、后台服务、自动提权、移动端和完整多显示器管理。架构应留下边界，但不为尚不存在的需求预建复杂框架。

## 2. 分层方向

```text
foundation -> protocol/core <- adapters
                      ^          ^
                      |          |
              application services
                      ^
                      |
             presentation / app
```

- `foundation`：稳定基础类型和小工具。
- `protocol/core`：类型化协议、会话状态、业务规则和外部端口。
- `adapters`：Windows、WebRTC、FFmpeg、QtNetwork、WebView2 等具体实现。
- application services：组合多个核心端口完成采集、会话、发现、设置、最近设备和剪贴板用例。
- `presentation`：Qt/WebView2/React 展示层，只提交用户意图。
- `app`：唯一组合根，选择实现、注入依赖和管理进程生命周期。

依赖必须指向核心抽象。核心不得包含 WebRTC、FFmpeg、DXGI、D3D、Win32 或 WebView2 类型。

## 3. 当前落地状态

工程已经通过 CMake 静态库形成实际边界，而不是把所有源码直接放入一个目标。主要目标和依赖见[架构概览](docs/architecture/overview.md)。多个 `.lib` 是编译期分层产物，最终仍链接为一个 `winRemoteControl.exe`。

已经完成的关键结构包括：

- `wrc_protocol`、`wrc_core` 与多组 application/adapter/presentation target。
- `KSessionCoordinator`、类型化状态机、结构化错误和 generation 隔离。
- DXGI CaptureSource、CaptureLoop/Service 与 FrameSink 解耦。
- `KRemotePeerTransport` 端口及 WebRTC adapter。
- 类型化协议路由、能力协商和独立 DataChannel。
- 异步停止、停止超时隔离、有限析构及 WebRTC 初始化失败事务回滚。

仍需持续收敛但不应一次性重写的集中点：

| 位置 | 继续演进方向 |
| --- | --- |
| `KWebRtcPeer` | 维持 PeerConnection 门面；只有出现独立变化原因时再提取 stats、SDP 或 channel 协作者 |
| `KSessionCoordinator` | 保持会话编排权威；避免重新吸收设备信息、平台输入或 UI 映射 |
| `KVideoRenderWidget` | 保留 Windows 原生渲染与事件入口；通用输入策略和统计应留在独立组件 |
| React 前端 | 逐步拆分 bridge、state、page 和 component，但不引入第二套业务状态机 |

## 4. 依赖与接口规则

1. 先定义 C++ 业务类型，再由 Codec 序列化；业务代码不散落手写 JSON key。
2. 只在真实外部边界或需要 fake 测试时抽象接口，禁止制造覆盖整个系统的 `IManager`。
3. adapter 不决定会话权限，presentation 不决定协议兼容性。
4. 第三方 include/link 只属于实际使用它的 target；默认使用 `PRIVATE` 依赖。
5. 发现核心 target 因公开头文件被迫链接 WebRTC、FFmpeg 或 Win32 时，应先修复边界。

## 5. 生命周期与线程规则

- 会话转换集中在状态机；停止、断开、窗口关闭和错误清理必须幂等。
- 所有异步回调携带 generation；旧会话、停止后和析构后的回调必须被 Gate 拒绝。
- GUI 线程禁止同步网络等待、无限线程等待和重型像素转换。
- worker 必须具有对称启动/请求停止/完成通知；异常析构只能有限等待，不调用 `QThread::terminate()`。
- 初始化必须“全部成功或回滚到 Idle”，半初始化资源不得复用。
- 高频帧与消息必须使用有上限队列、合并或丢弃旧状态的策略。

具体状态和回滚流程见[会话生命周期](docs/architecture/session-lifecycle.md)。

## 6. 协议与安全规则

- 线上消息校验版本、字段、大小、UUID/sequence 和当前状态权限。
- 被控端显式监听并审批后才能协商、采集和注入输入。
- 设备名称、React 数据和消息声明地址都不构成可信身份；来源地址以 socket 为准。
- 能力无交集时明确失败，不静默降级为猜测行为。
- 断线恢复只能延续同一条已审批会话；完整重连必须重新审批。
- 不记录按键内容、Unicode 正文、剪贴板正文或文件路径。

协议现状见[协议概览](docs/protocols/overview.md)。

## 7. 媒体与输入规则

- 采集源只产生平台无关帧，不判断 WebRTC 状态。
- 会话层控制媒体生命周期；渲染器不拥有 PeerConnection。
- 低延迟优先保留最新状态，不为完整播放过时帧而累积队列。
- 输入注入器只执行已审批、已校验且序号有效的类型化命令。
- 通道关闭、失焦、断线和恢复开始时必须释放已按下的键鼠状态。

视频实现见[视频链路](docs/media/video-pipeline.md)。

## 8. 可观测性与错误处理

- 同机耗时使用单调时钟；不能直接相减两台电脑的系统时间。
- 错误向上携带 domain、code、stage 和技术原因；presentation 决定用户文案。
- 可恢复、需用户重试和必须终止的错误必须进入不同状态。
- 高频日志采样或聚合，角色文件名明确，但日志只在本机落盘。

启动参数和事件说明见[日志与诊断](docs/development/logging-and-diagnostics.md)。

## 9. 测试与提交准则

- 纯逻辑优先单元测试：Codec、状态机、路由、协商、队列和坐标映射。
- 外部边界使用 fake/component 测试：TCP、发现、持久化、Capture 与 WebRTC 生命周期。
- 每项会话或媒体变更至少手测双进程、虚拟机或两台 Windows 主机中的适用场景。
- 不能以“编译通过”代替行为验证；异常路径包括端口占用、拒绝、断线、初始化失败和停止超时。
- 架构迁移一次只移动一个职责，保持协议和外部行为不变，并形成可单独回滚的提交。

构建命令与检查清单见[构建与测试](docs/development/build-and-test.md)。

## 10. 后续开发决策顺序

1. 先修复会破坏现有会话正确性、生命周期或安全边界的问题。
2. 再补足基础远控体验和可诊断性。
3. 新功能应落在已有分层；若接口不适合，先更新设计而不是跨层打补丁。
4. 只有真实需求和第二种实现出现后，才评估插件体系、跨平台或更通用框架。
5. 公网、账号、移动端等阶段变化必须单独重新评审威胁模型与部署架构。
