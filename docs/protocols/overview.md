# 协议概览

项目使用两层传输：Schannel mTLS over TCP 承载接入审批和 WebRTC SDP/ICE 信令；WebRTC 承载 H.264 视频及应用 DataChannel。React 不解析或构造线上协议。

## 类型化信封

应用消息先转换为 C++ 类型，再由 Codec 编码。通用信封语义为：

```text
version + channel + type + requestId/sequence + payload
```

Codec 校验版本、字段类型、UUID、数值范围和消息大小。当前 Session 能力协议范围为 `2..2`；缺少共同版本或必需能力时明确拒绝，不做静默猜测。

主要大小限制包括：接入消息 2 KiB、输入消息 4 KiB、Session 消息 160 KiB、信令消息 256 KiB、Clipboard DataChannel 消息 2 MiB、文件控制消息 128 KiB、文件数据帧 64 KiB。剪贴板正文自身最多 256,000 UTF-8 字节，Unicode 文本输入最多 2,048 字节。

## TCP 接入与信令

默认 TCP 端口是 `39000`。TCP 首先交换固定版本前导，之后必须完成 TLS 1.2/1.3 双向证书握手，不允许回退明文。TLS 内的接入阶段支持：

- `tlsPairingHello`（固定验证方法 `tls-exporter-numeric-v1`）、`tlsPairingDecision`、`tlsPairingReady`、`tlsPairingCommitted`、配对拒绝

- `accessRequest`
- `accessPending`
- `accessAccepted`
- `accessRejected`
- 前导状态 `OK/BUSY/INCOMPATIBLE`

设备身份固定和审批通过前到达的 SDP/ICE 被拒绝。之后 Offer、Answer 和 ICE Candidate 使用原有类型化 JSON framing，但只允许在已建立的 Schannel 通道内收发。TLS 提供保密性、完整性、顺序与防重放，不再存在逐条信令签名信封。

`Ready` 表示与 `requestId` 绑定的 Pending 信任已落盘；`Committed` 表示本地记录已经提交。只有双方都收到 `Committed` 后才进入身份认证成功状态。重复、旧 generation 或不匹配 requestId 的事务消息不会改变当前信任状态。

局域网发现是独立 UDP 协议，固定端口 `39001`。发现结果只提供连接候选，不等同于身份认证或接入授权。

## Session 能力协商

Session DataChannel 打开后交换 `KSessionCapabilities`，包括：

- 协议最小/最大版本。
- 支持的 codec 和 channel。
- 最大宽度、高度、FPS 与码率。
- 剪贴板、实时输入、键盘、Unicode、鼠标按钮与滚轮能力。
- 按钮按住期间切换到可靠输入通道的 `orderedMouseDrag` 可选能力。
- 显示器元数据；当前只描述，不提供显示器切换。
- 防窥模式 `supportedPrivacyModes` 与会话结束锁屏 `postSessionLock`。
- 双向文件传输能力及 `file-control`、`file-data` 通道。

必需交集为 H.264、`video`、`session` 和 `input`。`clipboard`、`input-realtime`、`file-control` 与 `file-data` 属于可选能力，不支持时不会阻止基础远控。文件传输只有在双方通道能力均存在且最终权限包含 `fileTransfer` 时才可用。

`orderedMouseDrag` 只有双方都声明支持且实时输入可用时才启用。字段缺失默认为关闭：空闲移动继续使用 `input-realtime`，按钮和旧版拖拽保持原兼容行为。启用后，按钮 `Down/Up` 与按住期间的移动统一走可靠 `input`；接收端会丢弃按钮按住期间迟到的实时移动，同时推进实时序号水位。

### 独立指针形状（GPU 发送路径仍在验收）

`cursorShapeV1` 是新增的可选布尔能力，缺失视为 false，只有双方都声明才得到交集。
Windows 应用在采集及原生显示模块就绪、最终权限包含 viewScreen 时发布该能力。
会话收发及原生指针显示已接线，双机视觉及真实输入仍需单独验收；该能力不授予输入权限。

形状消息类型为 `cursorShape`，走可靠、有序的 `session` 通道，复用 version=1
信封与十进制字符串 `seq`，不带 requestId，不使用会话命令 ACK。字段为 `kind`、
`width/height/stride`、`hotspotX/hotspotY`、`visible`、`offset/totalBytes`、`data`。
`kind` 取 0 默认箭头、1 预乘透明度的彩色 BGRA、2 单色 AND/XOR、3 MaskedColor。单色高度表示一层
掩码的逻辑高度，数据依次为等尺寸 AND、XOR 两层；MaskedColor 保留其特殊掩码语义，
不得视为普通透明度混合。消息不含远端鼠标位置或可改变本地授权的 generation。

宽高各不超过 256，stride 不超过 2048，完整原始数据不超过 256 KiB；每块最多
8192 字节，非尾块必须满块，offset 按 8192 对齐。数据为严格、规范 Base64，单条 JSON
仍受 Session 的 160 KiB 上限约束。默认箭头或隐藏默认指针允许零尺寸、零数据消息。
有形状时热点必须在可见尺寸内，尺寸与 stride 推导的数据长度必须与 totalBytes 一致。

重组器至多保留一份形状，缓存上限 256 KiB，首次分块后 5 秒超时。更高序号的首块可
替换未完成旧形状；乱序/重复分块、元数据变化会拒绝并清理本次重组，已完成/失效序号的
迟到消息忽略。会话层使用单次定时器清理无人继续发送的半份形状；暂停清理未完成数据但
保留序号水位，完整会话换代才 reset。发送端对采集事件重新分配会话内递增序号，避免
同一会话停止再启动采集时形状序号从 1 重来。接收限控制端、Streaming、当前 generation、
已协商能力且有 viewScreen 权限；发送限被控端同等授权状态。连续三次重组错误按协议错误
结束会话，像素不进入错误日志。

发送端只缓存最新一份形状（最多 32 个分块），未提交的旧形状可被替换；session 命令队列
优先，DataChannel 达高水位时停止提交，低水位回调恢复。停止推流、通道关闭和断开时清空。
旧客户端缺少能力时仍使用原 CPU 指针合成路径，新客户端不全局隐藏被控端鼠标。

控制端使用 Windows 原生光标：单色双掩码和 MaskedColor 保留背景 AND/XOR，彩色
DXGI 预乘 BGRA 转为原生光标所需颜色表示。形状尺寸、热点按原始物理像素保留，不随视频
缩小；Qt 逻辑坐标只用于判定光标是否位于视频内容区。离开视频区、隐藏窗口、停止和断开
恢复本地指针；默认/隐藏/不支持形状有明确退回，不调用全局 ShowCursor 或修改系统指针。

隐私控制使用 `setPrivacyMode`、`privacyModeState`、`setPostSessionAction` 和 `postSessionActionState`。命令复用 Session Command 的 UUID、1 秒超时、一次重试和结果缓存；状态事件始终携带远端真实 `effectiveMode`。旧客户端缺少能力字段时按仅支持 `disabled`、不支持结束锁屏处理。生产构建默认不发布 `displayoff`，因此控制端不会显示该入口。

## DataChannel

| Label | 可靠性 | 用途 |
| --- | --- | --- |
| `input` | reliable、ordered | 键盘、Unicode 文本、鼠标按钮；协商 `orderedMouseDrag` 后还承载按钮按住期间的移动 |
| `input-realtime` | unordered、`maxRetransmits=0` | 未按按钮时的高频鼠标移动等可被新状态覆盖的实时输入 |
| `session` | reliable、ordered | 能力、设备信息、流配置、开始/停止推流和结束会话 |
| `clipboard` | reliable、ordered | 双向纯文本剪贴板消息与同步状态 |
| `terminal` | reliable、ordered | ConPTY stdin 与合并后的 stdout/stderr 二进制字节流 |
| `file-control` | reliable、ordered | 目录分页、复制任务、累计 ACK、暂停、取消、冲突和完成状态 |
| `file-data` | reliable、ordered | 不含路径的文件二进制块 |

各通道有独立、有上限的发送队列和背压策略。输入序号用于拒绝重复或倒序事件；剪贴板使用 UUID 去重，日志不记录正文。

可靠拖拽移动只允许在同一按钮状态 epoch 内合并，不能跨越 `Down/Up`。按钮按下前清除尚未发送的实时移动，按钮全部释放后才恢复实时通道。更完整的输入所有权和兼容路径见[远程输入](../features/remote-input.md)。

终端控制消息通过 `session` DataChannel 传输，包括申请、审批、尺寸、关闭、退出和结构化错误；有副作用的命令具有 command ID、ACK、最多一次重试及幂等结果缓存。终端字节流不经过 JSON，每帧显式携带当前 terminal requestId 和单调 sequence；旧实例、重复和倒序数据会被拒绝。单帧总长最大 64 KiB，实际正文按 16 KiB 分块。`terminal` 是可选能力，没有交集时不影响桌面、输入和剪贴板。

文件传输生命周期消息通过 `session` DataChannel 传输，包括 `fileTransferOpenRequest`、接受、拒绝、关闭、停止和结构化错误。只有控制端按需创建 `file-control` 与 `file-data`；两条通道都打开后，文件会话才进入 Ready。创建失败只关闭文件传输，不结束远程桌面会话。

`file-control` 使用 `KFileTransferControlMessageCodec` 编码类型化 JSON。目录列表最多每页 256 项，跨端选择使用当前 generation 内有效的 `listingId + entryId`，不接受 React 直接提交来源路径。`file-data` 使用 `KFileTransferDataFrameCodec` 的固定大端二进制头，包含版本、flags、task UUID、file UUID、offset 与长度；路径和文件名不进入数据帧。正文按 32 KiB 分块，单帧总长不超过 64 KiB。发送端在 DataChannel 达到 1 MiB 高水位或应用层未确认数据达到 1 MiB 时暂停，低于 256 KiB 后恢复；接收端每实际落盘累计 256 KiB或文件结束时发送累计 ACK。

## 安全与兼容边界

- 设备名称只用于展示；可信身份是 CNG 私钥对应的固定公钥。
- 真实来源地址取 TCP/UDP socket。
- 新客户端要求能力协商；旧客户端三秒内得到明确不兼容错误。
- 协议拒绝不会降级为未校验 JSON，也不会绕过被控端审批。
- TCP 固定前导之外的身份、Access、SDP 和 ICE 均由 Schannel TLS 加密；WebRTC 媒体和 DataChannel 继续由 DTLS/SRTP 与 SCTP over DTLS 加密。
- `fileTransfer` 是独立权限；目录枚举、命令发送、命令接收和数据落盘均由 C++ 再次检查 generation、会话状态、协商能力和最终权限。
- 文件正文只经过 C++、磁盘和 `file-data`，不进入 React、JSON 或诊断日志。

文件传输的文件系统、安全提交和重连边界见[文件传输](../features/file-transfer.md)。
## H.264 SDP 能力更新（2026-10-10）

未新增私有信令字段或修改信令版本。H.264 工厂共享 Constrained Baseline Level 5.2
能力声明（`profile-level-id=42e034`），仅使用 packetization-mode=1，并关闭 level 非对称协商。
WebRTC 按双方能力得到共同等级；旧端只支持 3.1 时仍取 3.1，而不是强行向旧端发送 4.2。
编码配置不能满足协商约束时明确失败，不静默降低分辨率或 FPS。
接收实际 SPS 与协商约束不符时拒绝码流；因此旧程序曾经“声明 3.1、实际输出更高等级”
的连接可能被新版拒绝。建议两端同时更新，不能将此前宽松接受视为协议合规。
计算、后端配置和验证限制见 `docs/media/video-pipeline.md` 的 level 章节。
