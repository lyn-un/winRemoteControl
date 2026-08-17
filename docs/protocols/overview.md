# 协议概览

项目使用两层传输：Schannel mTLS over TCP 承载接入审批和 WebRTC SDP/ICE 信令；WebRTC 承载 H.264 视频及应用 DataChannel。React 不解析或构造线上协议。

## 类型化信封

应用消息先转换为 C++ 类型，再由 Codec 编码。通用信封语义为：

```text
version + channel + type + requestId/sequence + payload
```

Codec 校验版本、字段类型、UUID、数值范围和消息大小。当前 Session 能力协议范围为 `2..2`；缺少共同版本或必需能力时明确拒绝，不做静默猜测。

主要大小限制包括：接入消息 2 KiB、输入消息 4 KiB、Session 消息 160 KiB、信令消息 256 KiB、Clipboard DataChannel 消息 2 MiB。剪贴板正文自身最多 256,000 UTF-8 字节，Unicode 文本输入最多 2,048 字节。

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

必需交集为 H.264、`video`、`session` 和 `input`。`clipboard` 与 `input-realtime` 属于可选能力，不支持时不会阻止基础远控。

## DataChannel

| Label | 可靠性 | 用途 |
| --- | --- | --- |
| `input` | reliable、ordered | 键盘、Unicode 文本、鼠标按钮及需要可靠送达的输入 |
| `input-realtime` | unordered、`maxRetransmits=0` | 高频鼠标移动等可被新状态覆盖的实时输入 |
| `session` | reliable、ordered | 能力、设备信息、流配置、开始/停止推流和结束会话 |
| `clipboard` | reliable、ordered | 双向纯文本剪贴板消息与同步状态 |
| `terminal` | reliable、ordered | ConPTY stdin 与合并后的 stdout/stderr 二进制字节流 |

各通道有独立、有上限的发送队列和背压策略。输入序号用于拒绝重复或倒序事件；剪贴板使用 UUID 去重，日志不记录正文。

终端控制消息通过 `session` DataChannel 传输，包括申请、审批、尺寸、关闭、退出和结构化错误；有副作用的命令具有 command ID、ACK、最多一次重试及幂等结果缓存。终端字节流不经过 JSON，每帧显式携带当前 terminal requestId 和单调 sequence；旧实例、重复和倒序数据会被拒绝。单帧总长最大 64 KiB，实际正文按 16 KiB 分块。`terminal` 是可选能力，没有交集时不影响桌面、输入和剪贴板。

## 安全与兼容边界

- 设备名称只用于展示；可信身份是 CNG 私钥对应的固定公钥。
- 真实来源地址取 TCP/UDP socket。
- 新客户端要求能力协商；旧客户端三秒内得到明确不兼容错误。
- 协议拒绝不会降级为未校验 JSON，也不会绕过被控端审批。
- TCP 固定前导之外的身份、Access、SDP 和 ICE 均由 Schannel TLS 加密；WebRTC 媒体和 DataChannel 继续由 DTLS/SRTP 与 SCTP over DTLS 加密。
