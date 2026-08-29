# 独立帧率设置与高帧率支持方案

## 状态

- 当前仅记录方案，不修改代码。
- 控制中心 UI 等待 `theme-system` 分支合并后再实现，避免同时修改菜单样式和结构造成冲突。
- 建议按两个独立提交实施：先解耦画质与帧率，再扩展 90～144 FPS。

## 背景

目前项目已经能够通过 `KStreamConfig::nFps` 设置目标帧率，但帧率隐藏在画质预设中：

| 画质预设 | 当前目标帧率 |
| --- | ---: |
| 极速 | 60 FPS |
| 自动 | 30 FPS |
| 原画 | 30 FPS |
| 高清 | 30 FPS |
| 流畅 | 30 FPS |

现有配置会依次经过控制端菜单、Session 消息、能力约束、被控端采集、WebRTC 编码和接收端渲染，因此不需要为 60 FPS 以内的独立设置增加新协议消息。

当前协议约束、采集 Worker、帧转换、WebRTC H.264 编码器和接收端均将最高帧率限制为 60 FPS。90、120 和 144 FPS 不能只通过增加菜单项实现。

## 目标

- 将画质和帧率拆成两个相互独立的会话设置。
- 修改画质时保留当前帧率，修改帧率时保留当前分辨率和码率。
- 先稳定支持 30/60 FPS，再扩展到 90/120/144 FPS。
- 根据双方协商能力决定可选帧率，不向用户提供实际不可用的选项。
- 区分请求帧率、协商后的有效帧率和实际接收帧率，便于诊断。
- 不改变审批、权限、视频编码格式和 DataChannel 划分。

## 非目标

- 第一阶段不实现基于网络质量的自动升降帧率。
- 第一阶段不将帧率偏好写入配置文件，也不按设备持久化。
- 不保证软件编码器在高分辨率下能够稳定达到 90～144 FPS。
- 不在本次修改中增加可变刷新率、插帧或显示器切换。

## 阶段一：独立的 30/60 FPS 设置

### 菜单结构

在控制中心的“画质”子菜单中保留现有画质预设，并增加独立帧率分组：

```text
画质
├─ 极速
├─ 自动
├─ 原画
├─ 高清
├─ 流畅
├────────
└─ 帧率
   ├─ 60 帧
   └─ 30 帧
```

- 帧率选项使用互斥 `QActionGroup`。
- 当前目标帧率显示勾选状态。
- 新会话默认 30 FPS，与当前默认行为一致。
- 同一会话从 `Reconnecting` 恢复后保留已选择配置。
- 完整断开后，新会话恢复默认配置。

### 配置解耦

保留现有 `KStreamConfig` 作为唯一线上配置：

```cpp
struct KStreamConfig
{
	int nFps;
	int nWidth;
	int nHeight;
	int nBitrateKbps;
};
```

将窗口内部操作拆成两个明确入口：

- `applyQualityPreset(width, height, bitrateKbps)`：只修改分辨率和码率。
- `applyFrameRate(fps)`：只修改帧率。

每次修改后仍发送完整 `KStreamConfig`，继续复用现有 `StreamConfigSessionMessageType`，不增加协议类型。

### 能力约束

- 帧率选项不得超过 `KNegotiatedCapabilities::nMaximumFps`。
- 第一阶段双方最大值仍为 60，因此只显示或启用 30/60 FPS。
- Controller 发送前继续通过 `KMediaSessionController::constrainedConfig()` 约束配置。
- 被控端继续以收到的完整配置同时更新 Capture 和 WebRTC Sender。

## 阶段二：支持 90/120/144 FPS

### 菜单选项

```text
帧率
├─ 144 帧
├─ 120 帧
├─ 90 帧
├─ 60 帧
└─ 30 帧
```

- 高于协商上限的选项禁用或隐藏。
- 禁用项通过提示说明是远端能力、显示器刷新率或当前编码路径限制。
- 菜单勾选目标帧率；标题栏现有 FPS 指标继续显示实际接收/渲染帧率。

### 统一帧率上限

将各层写死的 60 FPS 收敛到共享约束，并把协议允许上限提升到 144：

- `KProtocolConstraints::kMaximumStreamFps`
- `KCaptureWorker::setStreamConfig()`
- `KCaptureFrameSink::normalizeStreamConfig()`
- `KWebRtcH264Encoder` 的帧率约束
- WebRTC Receiver 的 `max_framerate_fps`
- 其他存在本地 60 FPS 常量的发送和接收路径

所有层必须使用同一上限，避免菜单显示 144、但中间某层静默截断到 60。

### 本机能力发布

双方不应无条件声明支持 144 FPS。本机最大值应由以下限制共同决定：

```text
localMaximumFps = min(
    implementationLimit,
    activeMonitorRefreshRate,
    localDecodeOrCapturePolicyLimit
)
```

- 被控端以当前采集显示器刷新率和采集策略为主。
- 控制端以本地解码、渲染和显示刷新能力为主。
- 双方最终使用现有能力协商的最小值。
- 第一版仍固定采集当前默认显示器，不顺带实现显示器切换。

硬件编码与 `libx264` 软件回退的实际吞吐差异无法只靠静态能力完全判断。高帧率可作为目标值下发，但必须通过实际 FPS 和编码耗时日志判断是否跑满；若软件回退长期无法达到目标，再单独增加软件编码策略上限。

### 高精度采集节拍

当前采集节拍使用整数毫秒：

```text
1000 / fps
```

144 FPS 的理论间隔约为 6.944 ms，整数毫秒会产生明显误差。应改为基于单调时钟的绝对截止时间：

```text
nextDeadline += 1 second / targetFps
waitUntil(nextDeadline)
```

要求：

- 使用微秒或纳秒精度计算间隔。
- 处理单帧超时后跳过已经错过的截止点，不能连续补发形成突发流量。
- 修改帧率时从当前时间重新建立节拍，避免沿用旧周期。
- 停止和 immediate frame 请求仍能唤醒等待。

### WebRTC 与编码器

- `RTCRtpEncodingParameters::max_framerate` 使用有效目标帧率。
- WebRTC Video Sink wants 的最大帧率同步提高。
- H.264 编码器的 `time_base`、`framerate` 和 GOP 继续根据有效 FPS 初始化。
- 动态修改帧率时确认编码器是否需要重新初始化；不能只修改采集频率而保留错误的编码时间基。
- 码率与帧率保持显式独立，不在用户切换帧率时偷偷修改画质预设。高帧率低码率导致画质下降属于可观测取舍，后续可再设计智能码率策略。

## 状态与诊断

建议增加以下安全日志，不记录画面内容：

```text
stream_config_requested width=... height=... fps=... bitrateKbps=...
stream_config_applied requestedFps=... effectiveFps=... limitFps=...
```

诊断时区分三个值：

- `requestedFps`：用户选择值。
- `effectiveFps`：经过双方能力约束后的目标值。
- 实际 FPS：现有 WebRTC Stats / 渲染统计值。

当用户选择 144 FPS 但实际只有 70 FPS 时，可据此判断是能力截断，还是采集、编码、网络、解码或渲染性能不足。

## 预计修改位置

- `src/app/remotedesktopwindow.h/.cpp`
  - 菜单结构、互斥选项和配置字段解耦。
- `src/core/protocol/protocolconstraints.h`
  - 第二阶段提高协议帧率上限。
- `src/session/mediasessioncontroller.cpp`
  - 继续依据协商能力约束目标值。
- `src/session/sessioncoordinator.cpp`
  - 发布真实本机最大帧率能力。
- `src/capture/captureworker.h/.cpp`
  - 第二阶段提高上限并改为高精度节拍。
- `src/capture/captureframesink.cpp`
  - 使用共享帧率约束。
- `src/transport/webrtc/webrtch264encoder.cpp`
  - 使用共享上限并验证动态帧率更新。
- `src/transport/webrtc/webrtcpeer.cpp`
  - Sender/Receiver 最大帧率同步。
- 相应的 Protocol、Capability、Capture 和 Session 单元测试。

## 与主题系统分支的协作

`theme-system` 可能修改控制中心菜单样式或窗口主题。为减少合并冲突：

1. 主题分支合并前不修改 `KRemoteDesktopWindow::showControlCenterMenu()` 的 UI 结构。
2. 主题分支合并后，在最终主题 API 和菜单样式基础上实现阶段一。
3. 高帧率底层改造与主题无关，可作为阶段二独立提交。
4. 若主题系统将菜单构造从窗口类拆出，应在新的菜单组件中实现帧率选项，不恢复旧的硬编码样式。

## 测试计划

### 阶段一

- 修改画质不会重置当前帧率。
- 修改帧率不会修改当前分辨率和码率。
- 30/60 FPS 菜单只能单选，勾选状态正确。
- 超过协商上限的选项不可用。
- 切换帧率不重启会话、不重新审批。
- `StreamConfig` 消息仍能完整往返并在被控端生效。

### 阶段二

- 协议接受 30、60、90、120、144，拒绝非法值和超过上限的值。
- 双方最大帧率能力正确取交集。
- 高精度节拍在 90/120/144 FPS 下没有整数毫秒累计漂移。
- 修改帧率后编码时间基、WebRTC Sender 和采集节拍一致。
- 软件编码不能跑满时不阻塞 UI、键鼠和会话控制。
- 实际 FPS、编码耗时、丢帧及帧合并统计持续可用。
- Release 构建、全部 CTest、前端构建和 `git diff --check` 通过。

最终高帧率效果需要由用户在双机环境中验证，包括 60/90/120/144 Hz 显示器、硬件编码和 `libx264` 回退场景。

## 实施顺序

1. 合并 `theme-system`。
2. 实现画质与帧率解耦以及 30/60 FPS 菜单。
3. 完成构建和双机回归，确认现有 30/60 FPS 行为未退化。
4. 提高协议和各处理层上限，改造高精度节拍。
5. 开放 90/120/144 FPS，并按协商能力动态控制菜单。
6. 根据实机日志决定是否对软件编码回退增加单独上限。
