# 设备身份、mTLS 配对与权限

每个 Windows 用户拥有一份持久设备身份。ECDSA P-256 私钥由 Windows CNG 用户密钥库持有且不可导出；同一密钥对应的自签名设备证书保存在当前用户证书库。`security/device_identity.json` 只保存公开身份描述，`security/trusted_devices.json` 保存已固定的远端 SPKI 指纹与权限上限。两个 JSON 文件均由本机设备私钥签名并原子写入。

## 安全信令通道

现有 `QTcpSocket` 只负责字节收发，`KSchannelTlsEngine` 使用 Windows Schannel 在其上完成 TLS 1.2/1.3 双向证书认证和记录加解密。TLS 1.0/1.1 被禁用，握手后还会检查 ECDHE/AEAD 密码套件。除固定 8 字节版本前导及 `OK/BUSY/INCOMPATIBLE` 外，Access、SDP 和 ICE 都只在 TLS 内传输。

设备证书必须满足：当前有效、自签名可验证、ECDSA P-256、Digital Signature Key Usage、Client/Server Authentication EKU，以及包含合法设备 UUID 的 SAN URI。两端都必须在握手中实际使用对应私钥。

新证书的 `NotBefore` 会相对创建时刻向前回退 5 分钟，用于容忍局域网设备之间正常的小范围系统时钟偏差；`NotAfter` 使用可靠日期运算设为创建时刻五年后。程序在到期前 30 天使用同一 CNG 私钥续签，因此 SPKI 指纹和已有可信关系保持不变。证书尚未生效与已经过期会产生不同诊断信息，不会静默放宽超过容差范围的有效期校验。

## 首次配对

```mermaid
sequenceDiagram
    participant C as "控制端"
    participant H as "被控端"
    C->>H: TCP 固定版本前导
    C<<->>H: "Schannel mTLS（双方设备证书）"
    C->>H: tlsPairingHello(requestId, deviceId, verificationMethod, permissions)
    Note over C,H: "TLS Exporter 与规范化设备上下文生成相同六位数字"
    Note over C,H: "双方分别确认数字一致"
    C->>H: tlsPairingDecision（控制端确认）
    H->>C: tlsPairingDecision（被控端确认及权限上限）
    C->>H: tlsPairingReady（Pending 已原子写入）
    H->>C: tlsPairingReady（Pending 已原子写入）
    C->>H: tlsPairingCommitted（本地已提交）
    H->>C: tlsPairingCommitted（本地已提交）
    Note over C,H: 双方都收到 Committed 后才发布 authenticationSucceeded
    C->>H: accessRequest（TLS 内）
```

未知设备的 `tlsPairingHello` 必须声明 `verificationMethod=tls-exporter-numeric-v1`。两端使用 Schannel 的 TLS Keying Material Exporter，标签固定为 `EXPERIMENTAL-winRemoteControl-pairing-v1`；上下文按固定角色顺序包含协议版本、Access `requestId`、双方设备 UUID 和 SPKI SHA-256。导出结果的前 32 位映射为保留前导零的六位数字，例如 `482 731`。导出材料和数字均不通过协议发送，也不写入日志或存储。

两端均确认数字一致后进入独立的 `KPairingTransaction`。可信记录先以 `Pending` 状态和本次 `requestId` 原子写入；双方交换 `Ready` 后再提交为 `Mutual` 并交换 `Committed`。`commit()` 同一次安全关键保存会写入提交状态、证书摘要、展示名称和认证时间，成功后才允许发送 `Committed`。双方都收到同一事务的 `Committed` 后才发布认证成功；此后的统计性清理或可信重连时间更新采用 best-effort，写入失败只记录诊断，不能推翻已经成立的认证。

更新已有可信设备时，旧 `Mutual` 记录会与新事务记录同时保留到双方完成 `Committed`，随后才清理旧记录。程序若在 Prepare 或单端 Commit 后崩溃，启动恢复会删除 `Pending`，并在存在两个同公钥 `Mutual` 记录时保留较早、已经确认的旧记录；完整完成后的正常清理则只留下新记录。拒绝、超时、断线、TLS Exporter 不可用或任一安全关键写入失败都会恢复事务开始前的完整记录。

完整指纹格式仍为 `SHA256:<Base64(SHA256(SPKI DER))>`，但仅放在折叠安全详情中。后续连接比较设备 UUID、固定 SPKI 与双方提交 ID；同一 UUID 更换公钥时以 `device_key_changed` 拒绝，必须撤销后重新配对。

`tlsPairing*` 只负责交换验证方法、首次用户确认、权限选择和双方提交结果，不携带配对数字、TLS 导出材料、公钥、nonce、自定义 proof、配对秘密或签名。设备持有证明、加密、完整性、顺序及防重放全部由 Schannel TLS 提供。系统不支持 TLS Exporter 时明确返回 `channel_binding_unavailable`，不会降级为人工比较指纹。

## 信任库迁移

可信设备库格式为 v3，额外记录 `commitState` 与 `pairingTransactionId`。v1/v2 信任不能证明双方完成新提交协议：首次读取时原文件分别备份为 `.v1.bak`/`.v2.bak`，随后创建空 v3 信任库并要求重新配对。私钥和证书私钥从不写入该文件。

正式文件始终先写入同目录、随机命名的临时文件。临时文件完成 `QFile::flush()` 后还会对原生 HANDLE 调用 `FlushFileBuffers()`，再使用 `ReplaceFileW()`，或在首次创建时使用带 write-through 的 `MoveFileExW()` 完成原子替换；禁止直接截断旧文件。临时写入、系统 flush 或替换失败时保留上一份有效、已签名的信任库。这里保证的是文件内容完整、签名有效和名称替换原子；Windows 没有在此路径提供可移植的目录元数据同步语义，因此文档不宣称绝对的断电零丢失。

## 资源限制与错误边界

未认证连接的固定前导、TLS 握手密文、已加密信令和 socket 发送队列都有独立硬上限。监听 backlog、待处理连接和来源失败表也有固定容量。`KAdmissionController` 由 Transport 与 Authentication 共享：TLS 和 Pairing 失败进入同一个十分钟滑动窗口，且认证入口会在身份初始化和信任库读取之前检查来源限制；本机证书库、身份或文件系统错误不会误封远端来源。

安全失败由 `KSecurityStatus` 表达，包含稳定的 domain、code、stage、是否可重试、是否需要重新配对及内部技术信息。协议只发送有限拒绝原因；Win32/Schannel 细节只写本地诊断日志。

## 权限

最终权限是请求范围、可信设备权限上限、本次确认和能力协商的交集。C++ 在真实入口检查 `ViewScreen`、`InputControl`、`Clipboard` 和 `Terminal`；React 的按钮状态只用于展示，不能授予权限。终端即使具有设备级权限，每次打开仍需独立审批。
