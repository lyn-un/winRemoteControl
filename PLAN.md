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

## 11. 下一阶段：设备身份、配对与信令认证

当前 Access 审批只能证明“本次用户允许连接”，不能证明连接方是此前见过的同一台设备。设备名称由对端自行填写；WebRTC 虽使用 DTLS/SRTP 与 SCTP over DTLS，但 SDP 和 DTLS fingerprint 经未认证的 TCP 信令交换。下一阶段必须先补齐设备身份与信令完整性，再继续扩大远程终端等高权限能力。

### 11.1 安全目标

- 每台电脑拥有稳定 `deviceId` 和持久化设备密钥；私钥不得写入项目目录或普通配置文件。
- 首次连接通过两端可见的短验证码或公钥指纹完成配对，后续连接验证固定公钥。
- 随机 nonce、Access `requestId`、双方设备 ID、权限范围和当前 WebRTC 信令必须绑定到同一份签名 transcript，旧报文和旧会话信令不能重放。
- `autoAccept` 只适用于已配对、未撤销且请求权限不超过既有授权范围的设备；未知设备仍须首次配对。
- 权限在被控端业务入口强制执行，React 隐藏按钮不构成安全检查。
- 远程终端的设备级权限只表示允许发起终端申请；每次打开终端仍保留独立审批。

第一版解决身份认证和信令完整性。TCP 信令的元数据机密性不属于本轮目标；如后续需要隐藏 SDP、设备 ID 等信息，再独立评估 TLS。

### 11.2 分层与实现边界

- `core/security` 定义 `KDeviceIdentity`、`KPermissionScope`、`KTrustedDevice`、`KDeviceIdentityProvider` 和 `KTrustedDeviceStore`，不得包含 CNG、Win32 或 UI 类型。
- Windows security adapter 使用 Windows CNG 的 ECDSA P-256、SHA-256 和安全随机数；密钥由系统密钥存储持有并设为不可导出。
- `KDeviceAuthenticationFlow` 负责身份挑战、首次配对、nonce、签名校验和可信设备状态；由 `KAccessSessionFlow` 编排，但不把密码学细节放回 `KSessionCoordinator`。
- `KAuthenticatedSignalingFlow` 包装现有 Offer、Answer 和 ICE。WebRTC adapter 继续只产生和消费原始类型化信令，不直接访问身份存储。
- 可信设备记录保存设备 ID、固定公钥/指纹、用户别名、授权范围、配对时间、最后认证时间和撤销状态。对端声明的设备名称只能作为展示候选。
- 发现协议仍只产生连接候选；发现报文中的设备标识不构成可信身份。

签名输入必须使用固定字段顺序和长度前缀的规范化二进制编码，不能直接签名 JSON 文本或序列化 C++ 结构体内存。至少包含：协议域和版本、消息类型、Access `requestId`、双方 nonce、双方设备 ID、批准权限、sequence 和 payload SHA-256。

### 11.3 四阶段交付

1. **持久化设备身份与可信设备库**
   - 增加 core 接口、Windows CNG adapter、可信设备存储及签名/验签测试。
   - 复制 exe 或普通配置文件到另一台电脑时不得复用原设备身份；缺少对应系统私钥时生成新的 `deviceId + key`。
2. **Access 身份认证与首次配对**
   - 增加 `identityHello`、`identityChallenge`、`identityProof`、`identityAuthenticated` 和类型化拒绝消息。
   - 会话流程扩展为 `TCP Connected -> AuthenticatingIdentity -> Pairing(首次) -> Authenticated -> AwaitingApproval -> Negotiating`。
   - 两端基于同一 transcript 计算并确认六位配对码；首次配对增加来源级尝试限制。
3. **绑定 WebRTC 信令**
   - 对 Offer、Answer 和 ICE 外包认证信封，携带当前 Access `requestId`、发送方设备 ID、单调 sequence、transcript hash、payload hash 和签名。
   - 未认证、错误签名、篡改 SDP/fingerprint、重复/倒序 sequence 和旧 generation 信令必须在进入 WebRTC adapter 前拒绝。
   - 不支持身份握手的旧客户端在固定超时内明确提示升级，不允许降级为无认证连接。
4. **权限强制与可信设备 UI**
   - 授权拆为仅观看、键鼠、剪贴板和终端四项。
   - 采集、输入 Router/Injector、剪贴板服务和终端服务分别在被控端入口检查有效授权。
   - 设置页增加可信设备列表、指纹、授权范围、撤销和重新配对；审批卡片区分已验证设备与首次配对。

每个阶段形成可独立验证和回滚的提交；前三阶段全部完成前，不宣称已建立可信设备认证。

### 11.4 验收条件

- 已配对设备可认证连接；错误签名、公钥、nonce、`requestId` 和已撤销设备均被拒绝。
- 重放旧认证报文、修改权限范围、修改 SDP 或 DTLS fingerprint 后认证失败。
- 未配对设备不能触发 `autoAccept`，被撤销设备必须重新配对。
- 仅观看权限下视频可用，但键鼠、剪贴板和终端消息即使被伪造也由被控端业务层拒绝。
- 日志不得记录私钥、完整签名、配对码、认证令牌或敏感正文。
- Codec、身份流程、CNG/存储、信令绑定和权限检查均有自动化测试；两台 Windows 主机完成首次配对、后续认证、撤销和篡改失败的手动验证。
