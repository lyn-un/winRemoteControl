# winRemoteControl 后续开发与架构方针

> 本文是项目后续开发的长期约束，不是一次性重构任务清单。架构边界发生变化时，应先更新本文，再修改代码。

## 1. 目标与适用范围

当前阶段只建设“Windows 主机之间的局域网远程控制原型”。优先级依次是：

1. 会话可建立、可停止、断线后可恢复到明确状态。
2. 画面与鼠标、键盘输入正确可靠。
3. UI 不被网络、采集、编解码等耗时操作阻塞。
4. 延迟和故障链路可测量、可定位。
5. 在以上基础稳定后，再增加安全、体验和平台能力。

本阶段不引入公网中继、账号系统、文件传输、音频、后台服务、自动提权、动态插件系统，也不开始 Android、Linux 或 macOS 客户端开发。

架构要为未来变化留下边界，但不为尚不存在的需求实现框架。

## 2. 从 WPS 分层中借鉴什么

WPS 的价值不在于目录名称，而在于三件事：

- 基础设施、核心能力、应用外壳和扩展能力有单向依赖关系。
- 一个外部请求经过“传输 -> 协议解析/路由 -> 会话/命令 -> 具体产品能力”，每层只负责自己的语义。
- 分层由构建目标和依赖规则约束，而不只是把文件放进不同文件夹。

winRemoteControl 应采用同样的思想，但按当前规模缩小为：

```text
                         app（组合根）
                         /          \
          presentation /            \ adapters
       Qt / WebView2 / React          Windows / WebRTC / FFmpeg / QtNetwork
                         \          /
                    core（用例与策略）
             session / protocol / media / input
                              |
                    foundation（基础类型）
```

依赖方向不是简单的“上一层包含下一层”：

- `core` 定义业务规则和端口接口。
- `adapters` 实现 `core` 定义的端口，因此适配器依赖核心，而不是核心依赖具体技术。
- `app` 是唯一负责选择并组装具体实现的地方。
- `presentation` 只调用用例并展示状态，不拥有远程控制规则。

暂不复制 WPS 的 `plugin_support/plugin`。当前应先获得可替换接口；只有出现真实的运行时插件需求和第二种实现后，才评估动态插件。

## 3. 当前架构诊断

项目已经按 `capture`、`codec`、`input`、`session`、`transport`、`render`、`ui_bridge` 分了目录，这是正确的起点。但目录之间尚无强制边界。

### 3.1 构建层面

当前 `CMakeLists.txt` 将所有源码放进同一个 `winRemoteControl` 可执行目标，并一次性向所有代码暴露 Qt Widgets、Qt Network、WebRTC、FFmpeg、D3D、DXGI、Win32 和 WebView2。

后果是：

- 任何模块都能直接引用任何第三方或平台 API。
- 目录看似分层，编译器却无法阻止越层依赖。
- 纯状态机、协议解析等逻辑很难脱离完整应用做单元测试。
- 后续加入第二种实现时，容易继续通过条件分支堆进原类。

### 3.2 职责集中点

文件长度本身不是问题，多个变化原因集中在同一个类才是问题。目前最明显的集中点是：

| 位置 | 当前混合的职责 | 后续方向 |
| --- | --- | --- |
| `KWebRtcPeer` | WebRTC 工厂与线程、SDP/ICE、DataChannel、消息分类、视频收发、帧转换、网络统计、延迟 ping | 保留为 WebRTC 门面，逐步把协议分类、统计和媒体转换拆成协作者 |
| `KWebRtcSessionService` | 会话状态、TCP 信令协作、WebRTC 生命周期、JSON 协议、采集控制、输入注入、设备信息和壁纸读取 | 收敛为会话协调器；协议、平台信息和输入实现移出 |
| `KSessionViewModel` | UI 命令、对象创建与连线、输入 JSON 组装、端到端延迟统计、业务状态转发 | 只保留 UI 语义映射；对象所有权移到组合根，统计成为独立组件 |
| `KVideoRenderWidget` | D3D 渲染、帧队列、坐标换算、鼠标/键盘采集、输入节流、渲染反馈 | 允许作为 Windows 原生视图，但把通用输入策略和统计规则移出控件 |
| `frontend/src/main.jsx` | bridge 通信、全局状态、页面、组件和格式化 | 拆为 bridge、state、pages、components；仍然只做展示与交互 |
| `common` | 日志、帧水印、流配置等不同性质内容 | 停止作为兜底目录，按“基础类型/诊断/媒体”归位 |

已经出现的具体越层依赖包括：

- `KWebRtcSessionService` 直接包含 `Windows.h` 并读取桌面、主机和壁纸信息。
- `KSessionViewModel` 直接依赖 `KWebRtcSessionService`、WebRTC 统计类型和 `QJsonObject`。
- `KWebRtcPeer` 既理解 WebRTC 类型，又判断应用层输入消息类型。
- `ui_bridge` 直接暴露 WebRTC 命名的统计结构，导致界面协议绑定传输实现。

这些问题应渐进修复，禁止一次性大搬目录或重写现有可工作的音视频链路。

## 4. 目标分层

目标目录用于表达职责；迁移期间允许新旧目录暂时共存。

```text
src/
  foundation/
    types/              跨模块值类型、错误类型、时间与小型工具
    diagnostics/        日志接口、指标事件定义

  core/
    protocol/           类型化消息、校验、序列化、版本兼容与路由
    session/            会话状态机、角色、用例、停止/断线策略
    media/              帧与流配置模型、采集/发送/接收端口
    input/              鼠标键盘事件模型、输入授权与反馈关联

  adapters/
    windows/
      capture/          DXGI Desktop Duplication
      input/            Win32 SendInput
      render/           D3D11 渲染
      device/           主机、显示器、壁纸等系统信息
    webrtc/             PeerConnection、DataChannel、视频轨道和统计适配
    ffmpeg/             H.264 编解码实现
    signaling/          Qt TCP 信令实现
    diagnostics/        文件日志和性能日志实现

  presentation/
    qt/                 原生窗口和远程画面控件
    webview/            C++/WebView2 窄桥接与 UI DTO

  app/
    main.cpp            进程入口
    composition/        创建对象、选择实现、建立连接、管理顶层生命周期

frontend/src/
  bridge/               与 C++ 通信的唯一入口、消息校验
  state/                UI 状态与派生状态
  pages/                页面级组件
  components/           纯展示或轻交互组件

tests/
  unit/                 协议、状态机、坐标、统计等纯逻辑测试
  component/            单个适配器和线程生命周期测试
  integration/          双进程/回环会话测试
```

不要求立刻把每个目录建出来。没有代码的目录不创建；只有在职责真正被提取时才落地。

## 5. 各层依赖规则

### 5.1 `foundation`

- 可使用 C++ 标准库；迁移期允许必要的 QtCore 值类型。
- 不依赖 QtGui、QtWidgets、QtNetwork、Win32、WebRTC、FFmpeg、WebView2。
- 不知道“控制端/被控端”的具体业务流程。
- 一个类型只有被两个以上模块稳定共享，或确属全局基础语义时，才能进入此层。

### 5.2 `core`

- 只依赖 `foundation` 和自身子模块。
- 迁移期允许 QtCore 的对象、信号和 JSON 实现，但公开业务接口优先使用类型化结构，不能传裸 `QJsonObject` 表达业务含义。
- 禁止包含 `Windows.h`、WebRTC、FFmpeg、D3D、DXGI、Qt Widgets、WebView2 头文件。
- 定义“做什么”和“何时允许做”，不实现“用哪个系统 API 做”。

### 5.3 `adapters`

- 可以依赖 `core` 的接口和 `foundation`，并使用对应第三方库或 Windows API。
- 不决定会话业务状态，不把第三方类型泄漏到 `core` 或 UI 接口。
- WebRTC 层只负责可靠传送媒体和应用消息，不判断 `key`、`mouseMove`、`startStream` 等业务语义。
- Windows 输入适配器只执行已经授权、校验过的类型化输入命令。

### 5.4 `presentation`

- 可以依赖核心用例和 UI DTO，不直接调用采集、编码、信令或输入注入实现。
- React/WebView2 不保存权威会话状态；C++ 发出的快照和事件是事实来源。
- UI 文案和显示格式不能反向成为核心状态值。
- 原生远程画面控件可以包含渲染和原始事件采集，但不负责会话授权与网络协议。

### 5.5 `app`

- 是唯一的组合根，可以同时依赖 `core`、`adapters` 和 `presentation`。
- 只负责创建对象、注入依赖、连接信号、启动和关闭应用。
- 不解析协议、不实现会话策略、不做图像处理。

## 6. 核心请求链路

WPS driver 的“传输 -> 路由 -> 会话/命令 -> 产品能力”可对应为以下两条链路。

### 6.1 控制输入链路

```text
Qt 远程画面控件采集按键/鼠标
  -> ControllerSession 用例检查角色与会话状态
  -> InputMessageCodec 生成类型化、带 seq 的协议消息
  -> DataChannelTransport 发送不透明字节
  -> 受控端 ProtocolRouter 解析、校验并分发
  -> ControlledSession 再次检查会话和输入通道权限
  -> IInputInjector
  -> WindowsInputInjector / SendInput
```

关键规则：传输层不识别输入类型；注入器不解析 JSON；UI 不直接构造网络消息。

### 6.2 视频链路

```text
ControlledSession 发出开始采集命令
  -> IScreenCapture / DxgiScreenCapture
  -> 视频帧与输入反馈元数据
  -> IVideoSender / WebRTC 视频轨道与 FFmpeg 编码适配
  -> WebRTC 接收与解码
  -> IRemoteFrameSink
  -> Qt/D3D 远程画面控件完成渲染
  -> InputFeedbackTracker 记录端到端反馈
```

关键规则：会话层控制媒体生命周期；采集器不判断网络状态；渲染器不拥有 PeerConnection。

## 7. 类型、协议与接口方针

### 7.1 先类型化，再序列化

鼠标、键盘、会话控制、设备信息和流配置都应先定义 C++ 消息类型，再由 `protocol` 统一转成 JSON。业务层不得到处手写字符串键名。

建议的逻辑信封为：

```text
version + channel + type + seq/requestId + payload
```

这不是要求立即修改线上格式；第一步可以让新 codec 完全兼容现有 JSON，仅把构造、校验和解析集中起来。

协议层必须：

- 拒绝缺字段、字段类型错误、越界值和未知消息。
- 明确协议版本及向后兼容策略。
- 对输入、会话和诊断消息分别路由。
- 不在日志中记录键值、文本内容或其他敏感输入载荷。

### 7.2 只在真实边界引入接口

优先为以下外部边界定义小接口：

- `ISignalingTransport`
- `IRemotePeerTransport`
- `IScreenCapture`
- `IInputInjector`
- `IRemoteFrameSink`
- `IDeviceInfoProvider`
- `ISessionDiagnostics`

不要给每个类都造接口。只有满足以下任一条件才抽象：

- 隔离 Win32、WebRTC、FFmpeg、Qt Network 等外部实现。
- 已经有或即将有第二种实现。
- 为核心逻辑测试提供必要替身。
- 当前类有两个可以独立变化的责任。

接口应按调用者需求保持小而稳定，禁止建立一个包揽整个应用的 `IManager`。

### 7.3 核心状态必须类型化

逐步用以下类型替换跨层字符串：

- `SessionRole`
- `SessionState`
- `SignalingState`
- `DisconnectReason`
- `SessionError`（错误码、阶段、可展示描述）
- 与 WebRTC 无关的 `NetworkStats`

字符串只在日志、协议序列化或 UI 展示边界生成。

## 8. 生命周期、线程与所有权

远程控制最容易出错的不是“连上”，而是启动中取消、断线、重连和退出。

### 8.1 生命周期规则

- 会话状态转换集中到一个状态机，不允许多个类各自维护互相矛盾的布尔值。
- `stop`、`disconnect`、窗口关闭和错误清理必须幂等。
- 每次会话只能产生一次终止结果；过期定时器和旧连接回调必须带代次标识或在清理时失效。
- 输入通道关闭后立即拒绝新输入并释放被控端所有按下键。
- 受控端必须始终显示活动会话状态，并保留清晰可用的停止入口。

### 8.2 线程规则

至少明确以下执行域：

- GUI 线程：窗口、WebView2、D3D 控件事件和轻量状态展示。
- 采集线程：DXGI 抓帧与像素转换。
- WebRTC 内部线程：网络、worker、signaling。
- 编解码执行域：由适配器明确，不在 GUI 回调中等待。

跨线程约束：

- GUI 线程禁止 `waitFor*`、同步网络连接、阻塞式关闭和重型图像转换。
- 跨线程只传递拥有清晰生命周期的值对象或智能指针。
- Qt 连接必须明确使用直接还是队列语义；涉及大帧时要记录复制和背压策略。
- 每个 worker 都必须有对称的 `start/stop`，析构前可证明线程已经停止。
- 高频事件必须有有上限的队列、合并或丢帧策略，禁止无限积压。

### 8.3 所有权规则

- `app/composition` 创建长生命周期服务。
- `SessionCoordinator` 拥有一次远程会话的逻辑生命周期。
- ViewModel、窗口和 bridge 不创建网络或采集核心服务。
- QObject parent、智能指针和第三方引用计数对象不得混用出模糊所有权；每个成员在头文件中能说明谁销毁它。

## 9. CMake 必须成为边界检查器

最终目标不是立即生成很多库，而是逐步形成以下目标：

```text
wrc_foundation
wrc_protocol
wrc_core
wrc_windows_adapters
wrc_webrtc_adapter
wrc_ffmpeg_adapter
wrc_qt_presentation
winRemoteControl
```

目标依赖应满足：

```text
wrc_protocol          -> wrc_foundation
wrc_core              -> wrc_protocol + wrc_foundation
wrc_*_adapter         -> wrc_core + 对应外部库
wrc_qt_presentation   -> wrc_core + Qt UI
winRemoteControl      -> 上述具体实现（只做组装）
```

执行规则：

- 从最容易独立的叶子模块开始拆 target，不一次性重写 CMake。
- 第三方 include 和 link 只设在实际使用它的 adapter target 上。
- 默认使用 `PRIVATE` 依赖；只有公开头文件需要时才使用 `PUBLIC`。
- 若一个核心 target 因公开头文件被迫链接 WebRTC、FFmpeg 或 Win32，视为边界失败，应先修接口。
- 每次拆 target 后都完成 Release 构建和相应测试，再继续下一层。

## 10. 可观测性与错误处理

日志和指标是跨切面能力，不应散落为业务代码里的字符串拼接。

- 日志统一包含 `role`、`sessionId`、阶段、结果和必要耗时。
- 同机耗时使用单调时钟；禁止用两台机器的系统时间直接相减。
- 高频日志必须采样或聚合，并明确是否会影响时延。
- 会话、信令、媒体、输入使用稳定事件名，控制端和被控端文件名明确区分。
- 键盘日志只记录类型、按下/松开、序号和结果，不记录虚拟键值或文本。
- 错误从底层向上携带错误码、所属阶段和原始原因；只有 presentation 决定用户文案。
- 不得仅写日志后继续处于不明确状态；可恢复、需重试和终止错误必须有不同处理。

## 11. 前端与 C++ bridge 方针

WebView2/React 永远是展示层。

- bridge API 按用户意图命名，例如连接、停止、进入远程桌面，而不是暴露底层 socket 或 PeerConnection 操作。
- C++ 向前端发送版本化、类型化的 UI DTO；前端先校验消息再更新状态。
- React 中不得实现重连、超时、采集、编码、输入授权或会话终止规则。
- `main.jsx` 逐步拆成 bridge、状态 hook、页面和组件，但不为了拆文件引入复杂状态框架。
- C++ 保持唯一权威状态；页面重建时应能请求并恢复完整状态快照。

## 12. 测试策略

### 12.1 单元测试

优先覆盖不需要真实设备的纯逻辑：

- 协议消息编解码、缺失字段、越界和未知版本。
- 会话状态转换、重复停止、连接中取消、断线宽限和重连代次。
- 输入坐标映射、节流末次补发、按键按下集合和释放策略。
- 延迟滚动统计、序号跨越和无反馈记录上限。
- 流配置合法化与默认值。

### 12.2 组件测试

- TCP 信令的异步连接、超时、取消和重复连接。
- 采集 worker 启停、静态首帧重推和分辨率变化。
- 编码器硬件优先与 `libx264` 回退。
- 输入注入器在断线时释放按键。
- WebRTC adapter 的消息通道分类和关闭顺序。

### 12.3 集成与手工测试

保持两进程回环、虚拟机和两台真实 Windows 主机三种环境。每项影响会话或媒体的修改至少验证：

- 正常连接和主动停止。
- 连接中取消、目标不可达和异常断线。
- 静态桌面首帧、连续活动画面、分辨率变化。
- 鼠标最终位置、点击、滚轮、键盘与修饰键释放。
- UI 在连接、停止和网络异常期间持续响应。
- 首帧时间、输入到画面反馈延迟、帧率和丢帧没有非预期退化。

架构迁移提交必须在迁移前后跑相同验证，不能把“能编译”等同于“行为未改变”。

## 13. 渐进迁移路线

所有迁移都遵循：一次只移动一个职责；保持协议和外部行为不变；每一步可单独回滚。

### 阶段 A：建立基线和边界

- 记录当前主要链路、线程、对象所有权和手工测试基线。
- 给新增源码建立依赖审查规则，禁止继续扩大已知越层依赖。
- 新功能优先落到目标层，旧代码只在被触及时迁移。

完成标准：团队能回答每个长生命周期对象由谁创建、在哪个线程运行、由谁停止。

### 阶段 B：先提取协议层

- 建立类型化的输入消息、会话消息和设备信息消息。
- 集中现有 JSON 的构造、解析、校验和路由，保持线上格式兼容。
- 从 `KSessionViewModel` 移除输入 JSON 拼装。
- 从 `KWebRtcPeer` 移除应用消息类型判断；DataChannel 按固定通道交付原始消息。
- 为协议和路由补齐单元测试，建立 `wrc_protocol` 目标。

这是首个推荐执行的架构任务，因为它同时降低 ViewModel、SessionService 和 WebRtcPeer 的耦合，且不触碰 H.264 媒体实现。

### 阶段 C：提取会话状态机

- 将角色、状态、终止原因、连接代次和允许操作规则移入 `core/session`。
- `KWebRtcSessionService` 改为协调状态机与端口，不再自行组合大量布尔条件。
- 对启动中取消、重复停止、断线恢复和终止单次上报建立状态转换测试。

完成标准：不用启动 WebRTC，也能完整测试会话决策。

### 阶段 D：隔离 Windows 能力

- 从 SessionService 移出电脑名、屏幕信息和壁纸读取，放入 `WindowsDeviceInfoProvider`。
- 用 `IInputInjector` 隔离 `SendInput`。
- 明确 DXGI capture 和 D3D renderer 的平台适配边界。
- 核心公开头文件中不再出现 `Windows.h`、D3D 或 DXGI 类型。

完成标准：`wrc_core` 不需要 Windows SDK 即可编译其纯逻辑部分。

### 阶段 E：收敛 WebRTC 适配器

- `KWebRtcPeer` 保留 PeerConnection 门面和第三方回调适配。
- 将网络统计/延迟 ping、远端帧排队与转换、DataChannel 包装按变化原因拆成协作者。
- 让 SessionCoordinator 依赖 `IRemotePeerTransport`，避免暴露 WebRTC 枚举和统计结构。
- 保持当前 SDP、H.264 编解码、0/0 ms 播放延迟和软件编码回退行为不变。

完成标准：核心会话逻辑可用 fake transport 测试，WebRTC 类型只存在于 adapter 和组合根。

### 阶段 F：瘦身 ViewModel 与应用外壳

- 将服务创建和顶层信号连接移到 `app/composition`。
- 将输入到画面反馈统计移到独立 tracker。
- ViewModel 只把 UI 意图转为用例调用，并把核心状态映射为 UI DTO。
- MainWindow 只管理窗口导航和原生控件装配。

完成标准：ViewModel 不依赖 `KWebRtc*`、不构造 JSON、不拥有 capture/transport 实现。

### 阶段 G：整理前端

- 提取 C++ bridge client 和消息 schema。
- 拆分 dashboard、远程桌面、状态卡片和控制项。
- 保持现有外观与行为，不在结构提交中同时做大规模 UI 改版。

完成标准：页面组件可在 mock UI 状态下渲染，bridge 只有一个入口。

## 14. 新功能落位检查表

提交设计或代码前，依次回答：

1. 这是用户界面、业务规则、协议、第三方适配，还是操作系统能力？
2. 它应该依赖谁？是否出现核心反向依赖具体实现？
3. 是否在 UI 或 transport 中重复维护了会话状态？
4. 是否通过裸 JSON、字符串状态或第三方类型跨越了模块边界？
5. 是否有明确的启动、取消、停止、失败和析构路径？
6. 是否会阻塞 GUI 线程或建立无上限队列？
7. 日志是否足够定位，又不会泄露输入或刷屏？
8. 最小可验证成功标准是什么？需要哪一级测试？
9. 这次抽象是否服务于真实边界，而不是猜测未来？
10. 是否能拆成行为变更和结构迁移两个独立提交？

对应落位原则：

| 问题性质 | 放置位置 |
| --- | --- |
| 页面、按钮、文案、展示格式 | `frontend` 或 `presentation` |
| 会话允许做什么、状态如何变化 | `core/session` |
| 消息结构、校验、兼容和路由 | `core/protocol` |
| 输入与媒体领域模型 | `core/input`、`core/media` |
| Win32/DXGI/D3D/系统信息 | `adapters/windows` |
| WebRTC/FFmpeg/Qt TCP | 对应 `adapters` |
| 对象选择、创建和连接 | `app/composition` |
| 真正跨领域且无外部依赖的稳定类型 | `foundation` |

## 15. 明确禁止的做法

- 只移动目录和重命名，不改变依赖边界。
- 新建笼统的 `Manager`、`Utils`、`Common` 并继续堆功能。
- 为可能永远不会出现的实现建立多层抽象或动态插件系统。
- 在 React 中补写 C++ 会话逻辑，或在 WebRTC adapter 中补写业务路由。
- 用全局单例解决对象传递、线程或生命周期问题。
- 在一次提交中同时重构协议、媒体链路、UI 和行为。
- 因为“未来可能支持 Android”而现在引入移动端代码；当前只保证核心边界不泄漏 Windows 实现。

## 16. 后续开发顺序

在 Windows 基础功能尚未稳定前，优先顺序固定为：

1. 会话生命周期、断线和停止可靠性。
2. 输入与画面正确性。
3. 异步性、背压、首帧和延迟表现。
4. 协议校验、受控端可见授权与基础安全加固。
5. 自动化测试和诊断能力。
6. 体验增强和新平台。

下一项架构工作建议从“阶段 B：协议层提取”开始，而不是先拆最大文件或创建插件框架。它的收益可测试、风险较低，也会为后续会话状态机和其他平台适配建立稳定边界。
