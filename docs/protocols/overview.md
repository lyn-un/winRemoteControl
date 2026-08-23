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
- 显示器元数据；当前只描述，不提供显示器切换。
- 防窥模式 `supportedPrivacyModes` 与会话结束锁屏 `postSessionLock`。
- 双向文件传输能力及 `file-control`、`file-data` 通道。

必需交集为 H.264、`video`、`session` 和 `input`。`clipboard`、`input-realtime`、`file-control` 与 `file-data` 属于可选能力，不支持时不会阻止基础远控。文件传输只有在双方通道能力均存在且最终权限包含 `fileTransfer` 时才可用。

隐私控制使用 `setPrivacyMode`、`privacyModeState`、`setPostSessionAction` 和 `postSessionActionState`。命令复用 Session Command 的 UUID、1 秒超时、一次重试和结果缓存；状态事件始终携带远端真实 `effectiveMode`。旧客户端缺少能力字段时按仅支持 `disabled`、不支持结束锁屏处理。生产构建默认不发布 `displayoff`，因此控制端不会显示该入口。

## DataChannel

| Label | 可靠性 | 用途 |
| --- | --- | --- |
| `input` | reliable、ordered | 键盘、Unicode 文本、鼠标按钮及需要可靠送达的输入 |
| `input-realtime` | unordered、`maxRetransmits=0` | 高频鼠标移动等可被新状态覆盖的实时输入 |
| `session` | reliable、ordered | 能力、设备信息、流配置、开始/停止推流和结束会话 |
| `clipboard` | reliable、ordered | 双向纯文本剪贴板消息与同步状态 |
| `terminal` | reliable、ordered | ConPTY stdin 与合并后的 stdout/stderr 二进制字节流 |
| `file-control` | reliable、ordered | 目录分页、复制任务、累计 ACK、暂停、取消、冲突和完成状态 |
| `file-data` | reliable、ordered | 不含路径的文件二进制块 |

各通道有独立、有上限的发送队列和背压策略。输入序号用于拒绝重复或倒序事件；剪贴板使用 UUID 去重，日志不记录正文。

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
