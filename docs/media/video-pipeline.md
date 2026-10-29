# 视频链路

## GPU 发送路径实现状态

目前默认仍使用下文的 BGRA→I420→NV12 发送链路。实验接入已包含：
DXGI/HDR 输出复制到 4 槽自有 BGRA GPU 池，原生 WebRTC 缓冲保持共享所有权，
编码线程 GPU 缩放/转换到 8 槽 NV12 硬件帧池，再交给已选定的 QSV/AMF/NVENC。
转换位于 WebRTC 编码适配层内部，以便按实际选定后端创建资源，不为启动先读回首帧。
编码器仅在第一张图像提交前切换输入配置，保留后续码率/IDR/PTS 管理。

`--video-frame-path=cpu|auto|gpu` 本轮评估后保持 `cpu` 为默认：NVENC 1080p60节拍未全面
改善，且GPU-CAP-001仍独立待查。GPU改造以显式auto/gpu入口交付，不等于默认全面启用。
双方新版协商指针能力后，`auto` 可尝试 GPU 采集；旧端不支持时 auto 保留 CPU 路径，
强制 `gpu` 会报告缺少指针分离能力。硬件/适配器不兼容仍按各模式的回退或失败策略处理。

指针分离的基础类型和分块编解码已加入；DXGI 在分离状态下只对形状/可见性变化产生
独立更新，位置变化不产生新形状。单色指针双高度被规范化为逻辑高度与 AND/XOR 数据，
不支持或超限的形状退回默认箭头。worker 即使在指针专属 Timeout 上也转交形状，
service 只保留最新待转交项并检查来源 generation，停止时清理。应用发布可选
`cursorShapeV1`；会话层按观看权限和当前 Streaming 状态收发，控制端原生光标保留
掩码/透明度及物理热点。新新 CPU 会话也分离指针，使 CPU/GPU 对照采用相同行为；
旧对端继续原 CPU 合成。具体边界见[协议概览](../protocols/overview.md)。

真实运行范围：已用 Intel 被控端强制 QSV GPU 完成只读双机基础回归，1152×720 到
1920×1200 的配置变化、可见合成动画、同会话停止/恢复和断开通过；日志确认无整帧读回、
无CPU转色且采集池有界。另以Intel控制本机、实际NVIDIA采集适配器完成NVENC强制GPU
反向基础回归，1280×720到2560×1440及20次同会话停止/恢复通过；这是启停，不是20次
重新接入。NVENC另完成1808.2秒只读长稳，89个采样帧数及时间戳持续增长且无当前错误，
随后正常断开；资源日志仅生命周期快照，不证明运行期间峰值或无泄漏。
随后使用同一新版程序完成实际 HDR 基础复验：AMD 主机上 source=2560×1440、
inputFormat=10（FP16）、hdrGpu=1、白电平2.5、采集/NVENC LUID均111810，
readbackBytes=0、cpuConversion=0；Intel 接收从1280×720切到2560×1440并持续增长。
约56秒采集1675帧、错误0；不是新的30分钟HDR长稳、主观画质或完整色调映射验收。
实际 AMF auto 会话也已确认 NVIDIA 采集111810/AMD编码103674不匹配时明确记录
cross_adapter，CPU桥接后持续传输；这不是AMF跨卡GPU支持。另以可见只读本机回环确认
强制gpu以同一cross_adapter原因失败，输出/接收帧均0，没有静默转CPU。
AMF回退实际观察90样本、1829.75秒，首末样本跨度1809.44秒，帧数/时间戳持续增长；
最终采集/编码错误均0。观察脚本最后读取未保活的宿主driver session因30分钟空闲过期失败，
所以保留脚本退出1，不写成“脚本全通过”。末段21个资源样本不是全程峰值或无泄漏证明。
真实点击/滚轮和125%DPI下基础指针变化已观察；物理键盘与100%/150%DPI由用户明确暂缓。
默认路径仍不因此改为 auto。
AMF另在本机正常配对的只读回环完成20次同会话停止/恢复，每次停止安静窗口和恢复新帧
均通过，最终正常断开、退出；实际为跨卡CPU回退，不称为20次完整重连或GPU跨卡验收。
此门槛防止新路径在旧对端出现双指针，不代表整项改造已经完成。
合成帧测试已验证三后端的原生缓冲→GPU 转换→编码→解码，不能代替双机验收。

原生缓冲的兼容读回/缩放入口现补充 generation 检查：旧会话帧不得返回旧 I420 缓存或
触发新的兼容转换；转换结束后再次检查当前代际与CPU回退状态。对应旧缓存/旧缩放回归
已通过普通Release构建及完整CTest中的原生兼容回归；该边界验证不等同新一轮双机长稳。

同卡路径使用同一 D3D11 设备及递归 context 锁；硬件帧引用释放前不回收 NV12 槽。
BGRA 池只供 GPU 输入复制读取，生产者后续复用命令在同一 immediate context 上排在
消费者复制之后；编码器异步读取的是独立 NV12 池。QSV 设备要求 VIDEO_SUPPORT
与原生多线程保护。共享设备缓存现在按所需 VIDEO_SUPPORT 检查；若预览先创建了
不具备该标志的设备，GPU 采集会先创建新设备，成功后替换缓存，旧设备仍由旧持有者管理。
启动顺序修正及专用设备探针已在本机、Intel端显式通过；Intel新版另完成20次完整
重连及1816秒回环观察，88个样本帧数/时间戳严格增长，采集和编码错误均0。
合成窗口约10Hz变化，不将该观察视为30FPS能力证明；也不是双机长稳。
此前超过两小时观测缺口后发生过capture_failed，原始错误未保留，根因仍未知；
新一轮通过不代表已定位或修复该失败。
后续双机QSV输入观察又在约682秒发生取帧失败，明确为AcquireNextFrame 0x887a0001，
编码7247提交/输出且编码错误0。现补充ReleaseFrame检查及获取/释放计数，专项fake回归已过；
这只是补齐失败边界，尚不宣称该真实失败根因已修复。
跨卡目前明确返回不兼容，`auto` 保留所选后端并经 CPU 桥接，
不将此回退描述为跨卡 GPU 支持。运行中 GPU 失败锁定失败并由采集错误收尾，不逐帧切换。

NVENC 的固定 FFmpeg 版本读取私有 `level` AVOption；应用同时下发既有 level 预算，
避免小尺寸时驱动自动选择过低 SPS level。协商策略和接收端 SPS 校验未放宽。
具体运行方法与覆盖边界见[构建与测试](../development/build-and-test.md#gpu-硬件帧接入专项发送链路尚未启用)。

## 独立待办 GPU-CAP-001：QSV 会话偶发 DXGI 取帧失败

状态：用户同意与本轮GPU改造分开跟踪，未宣称修复。2026-10-27的Intel被控双机会话
在约682秒后记录`AcquireNextFrame failed (HRESULT=0x887a0001)`，编码器7247次提交与
7247次输出、编码错误0；控制端收到`remote_capture_failed`。不是已证实的QSV编码故障。

[Microsoft文档](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe)
将这个返回码与前一帧未释放关联，但当时没有保留ReleaseFrame的返回结果，不能据此断言
具体漏调用位置或驱动原因。新检查记录释放结果和获取/释放计数，释放失败立即终止采集；
这补齐了诊断和错误传播，并不是根因修复证明。不添加无限重试、不绕过锁屏或系统保护。

本轮保留CPU默认路径及显式auto/gpu开关；后续应使用该失败详情定位并独立复验。
物理键盘、100%/150%系统DPI由用户明确暂缓，不记为已通过。
2026-10-28新程序另完成一次855秒只读定向观察：采集成功8451帧、QSV8374次提交/输出，
采集及编码错误0，未复现旧错误；正常断开后两端程序退出。此有限观察不证明根因修复，
GPU-CAP-001仍保留为独立待办。合成窗口约10Hz变化，不以收帧数声称30FPS吞吐。

2026-10-28继续排查时，读取Intel本机Windows事件日志发现：原采集错误时间为
2026-10-27 22:41:09.934，Winlogon/Operational在22:41:09.935–.937记录811/812、
通知Event=4（Sens/TermSrv）；此前22:29:08.558为Event=5。
这提供了桌面/会话切换的毫秒级时间关联，应优先验证锁屏边界，而不是将故障归到QSV。
当时尚未通过受控锁屏复现确认因果，不能据此宣布漏释放已排除或驱动故障已确认。
复验只由用户手动锁定/解锁，不自动操作登录或捕获安全桌面。
进一步核对另一历史会话`gpu-intel-loopback-20261027c/host`：采集在21:11:23.704
收尾报告1次错误，Winlogon在21:11:24.637记录Event=5，随后21:11:33.933记录Event=4。
第二个独立故障也邻近会话切换，但旧日志未保存原始HRESULT，不能称同一返回码已复现。

同日新一轮观察记录183.08秒内9次收帧增长，随后本机观察进程意外结束；
远端在202.076秒收尾，采集1876次成功/0错误、QSV1758次提交/输出/0错误，
断开原因为ice_disconnected_timeout。观察进程退出原因未确定，这不是计划的20分钟
完整通过，也不是新的DXGI错误复现。原始记录保留在本机构建目录。

2026-10-29用户手动锁屏/解锁的受控测试确认一条断连原因：00:02:06.069
`ReleaseFrame`返回`0x887a0026`（DXGI_ERROR_ACCESS_LOST），记录acquired=764、
released=763；Winlogon在00:02:06.073–.076记录Event=4，在00:02:12.640后记录
Event=5。此前画面持续增长，QSV最终732次提交/732次输出、编码错误0。
最后一帧确实调用了ReleaseFrame，但复制对象因桌面切换失效，当前worker将该错误
上报capture_failed并结束会话；没有重建duplication后等待普通桌面恢复的状态分支。
该计数差不等同于漏调用。测试没有尝试解锁、访问或捕获安全桌面，结束后测试程序均退出。

本次复现的是释放阶段ACCESS_LOST，不是原历史获取阶段INVALID_CALL；结合历史事件关联，
桌面切换是已确认的断连触发条件，但历史0x887a0001的具体内部状态仍不可追溯证实。
后续修复应单独设计受限的采集对象失效处理，并保留现有授权与安全桌面边界；本轮只排查，
不自动实现恢复，不将锁屏复现写成已修复。

## 接收端窗口像素尺寸

接收端 D3D11 交换链和视口按视频 HWND 的客户区像素尺寸同步，不直接使用 Qt
逻辑尺寸。客户区查询在该 HWND 的 DPI 感知上下文内执行，并立即恢复线程上下文；
不修改进程 DPI 策略。Qt 布局和鼠标映射继续使用逻辑坐标，I420/BGRA 的双线性
过滤保持不变。尺寸未变化时复用缓冲区；空客户区或最小化时不提交绘制。

DPI/屏幕变化后排队重绘，正常 resize、恢复显示及绘制入口也会同步尺寸。
这项修复针对高 DPI 下潜在的额外拉伸，不代表编码损失、色度采样或缩小导致的
全部模糊均已解决。Intel 控制 AMD 的原画会话已确认尺寸同步，用户反馈清晰度改善；
反方向修复后回归、量化画质 A/B 和不同物理显示器间的 DPI 切换仍待验证。

## 总览

```mermaid
flowchart LR
    DXGI["DXGI Desktop Duplication"] --> BGRA["KCaptureFrame / BGRA"]
    BGRA --> LOOP["CaptureLoop / KCaptureService"]
    LOOP --> SINK["KCaptureFrameSink"]
    SINK --> I420["FFmpeg swscale / I420"]
    I420 --> SOURCE["WebRTC VideoSource"]
    SOURCE --> ENC["H.264 encoder"]
    ENC --> RTC["WebRTC video track"]
    RTC --> DEC["H.264 decoder"]
    DEC --> PROCESS["RemoteFrameProcessor / shared I420 or BGRA fallback"]
    PROCESS --> D3D["KVideoRenderWidget / D3D11"]
```

## 被控端

`KDxgiDesktopDuplicator` 从当前默认输出采集桌面和指针，生成与 WebRTC 无关的 BGRA `KCaptureFrame`。CaptureLoop 负责帧率、超时和取消；`KCaptureService` 管理线程、generation、Sink 选择和停止。

HDR FP16 输入默认先经过 `KHdrGpuBgraConverter` 离屏绘制：复制到可复用的 FP16 SRV 纹理，
着色器按像素读取、除以固定白电平 2.5 并裁剪，以线性 RGB 写入 sRGB RTV。硬件进行 sRGB
曲线编码，输出 BGRA8 再复制到 CPU staging。有效读回图像数据为 4 B/像素，旧 CPU 路径为
8 B/像素；该数值不是总线实测流量。鼠标仍在 CPU 的 BGRA 帧上合成，后续缩放和编码不变。

`--hdr-conversion=cpu` 保留旧标量转换用于对照；默认 `auto` 在 GPU 专属初始化失败后，
该采集初始化周期固定回退 CPU，不逐帧重试。设备移除和公共 Map 失败仍报告采集错误。
NaN/负无穷转黑、正无穷转白；普通有限值沿用旧算法。固定白电平与高光裁剪的色彩限制
没有修复，这不是 HDR 端到端传输或完整色调映射。

采集共享设备不再声明 SINGLETHREADED；共享 immediate context 的复制、转换、读回及
关闭 Flush 使用同一 mutex 串行化，不覆盖 AcquireNextFrame 等待。转换器只使用采集上下文，
显式设置并解除自身资源绑定，不清除其他使用者的整套状态。性能及真实桌面验收状态见
[HDR GPU 转色方案](../plans/hdr-gpu-capture.md)。

WebRTC FrameSink 使用 FFmpeg `swscale` 按协商流配置缩放并转换为 I420。I420 帧通过核心媒体类型交给 WebRTC VideoSource，因此 DXGI adapter 不需要包含 WebRTC/libyuv 头文件。

静态桌面启动时，首帧后的短保护期可重推最近帧并刷新时间戳，避免软件编码器尚未输出时长期黑屏。保护期结束后仍以桌面变化驱动为主，不持续发送重复帧。

## H.264 编码

默认 `auto` 按 DXGI 枚举顺序生成硬件候选：Intel 使用 `h264_qsv`，AMD 使用
`h264_amf`，NVIDIA 使用 `h264_nvenc`，跳过软件适配器。当前采集使用默认 D3D11
硬件适配器和输出0，该默认设备对应 DXGI 首个适配器；因此它若支持厂商编码会优先
尝试，否则依次尝试其他支持的硬件。每个候选用本次分辨率、帧率、码率和 level 实际
初始化；全部失败或没有候选才回退 `libx264`。失败上下文及其硬件设备引用在下一次
尝试前释放。MF 不再参与自动选择，仅 `--diagnostic-video-encoder=h264_mf` 显式可选。

硬件设备以枚举索引打开，并核对实际设备 VendorId/LUID，避免多张同厂显卡时绑定错误。
显式 QSV/AMF/NVENC 选择对应首个硬件候选，失败直接报告，不切换后端；运行中编码错误仍沿用
原错误处理，不在会话中途切换编码器。未来若支持选择采集显卡/输出，必须显式传递其
身份，不能继续假定采集使用默认适配器。软件路径使用 `ultrafast`、`zerolatency`、
低延迟、无 B 帧和受控 GOP。

WebRTC 输入为 I420，编码器内部转换为 NV12。线上保持 H.264 和既有 SDP/Annex-B 处理，控制端不需要知道硬件或软件编码器的选择。

NVENC使用绑定到候选显卡的D3D11VA设备及CPU NV12输入，配置p4/ull、CBR、无B帧、
关闭lookahead、zerolatency=1、delay=0、forced-idr=1、baseline；输出须通过既有SPS检查。
VBV为目标的250ms比特预算（最小1000bit），运行中更新平均/峰值码率和VBV。当前固定
FFmpeg动态重配置默认会resetEncoder并forceIDR，频繁更新可能增加关键帧和峰值、降低画质。
项目补丁为H.264纯码率重配置增加`bitrate-reconfig-reset`与`bitrate-reconfig-idr`私有AVOption，
范围0/1，默认均为1。`KH264Encoder`可按实例在打开前显式设置；显式策略要求手动选择NVENC，
避免自动回退后把其他编码器的结果当作NVENC测试。旧DLL不认识选项时明确拒绝显式策略。
应用在检测到两个选项均存在时默认设置0/0；显式测试配置可覆盖。
旧依赖缺少选项时使用库默认1/1，并记录`options_unavailable`，显式实验仍报错。
HEVC/AV1及同时改变DAR的更新保持上游策略。
既有`forced-idr=1`继续用于显式关键帧请求，与新增选项独立。
重配置失败仍由FFmpeg日志报告，字段更新不等于硬件接受；专项测试捕获该错误并判失败。
上游函数的重配置参数指向内部配置，失败时不能假定缓存已回滚，本补丁没有改变该错误恢复机制。

`libx264` 从初始化起启用 VBV：平均码率和最大码率等于当前目标码率，缓冲为 500 ms 的比特预算（最小 1000 bit，避免 FFmpeg 转换成整 kbit 后关闭 VBV）。未指定初始码率时沿用原有分辨率/帧率默认值。运行中同步更新这三个参数，由 FFmpeg 在后续编码时重配置，不重建编码器、不额外请求关键帧。相同目标直接返回；非正目标保持上次有效设置，超出 VBV 整数字段范围的目标拒绝更新。500 ms 是编码预算，不是新增播放等待；不启用 filler，输出不保证填满目标码率。

这修复了旧软件路径仅更新 `bit_rate`、未开启 VBV 时实际输出不跟随目标的问题，但更严格的突发约束可能降低复杂关键帧的质量。旧 MF 动态补丁未通过硬件验收，活动构建和应用专属桥接已移除，MF 保留初始化码率并报告运行时更新不支持；默认选择改为上述厂商路径，MF 本身未修复。

QSV/AMF 厂商路径采用 NV12、无 B 帧、浅异步队列，CBR 目标与峰值相同，显式缓冲预算为 250 ms（不是播放等待）；AMF 严格低码率精度仍有已知失败，纳入默认候选不代表该限制修复。QSV 复用上游 reset，AMF 使用已获确认的最小属性更新补丁。适配层按输出 PTS 关联 RTP 时间戳、采集时间和色彩元数据，待输出记录上限 64，不保留原始整帧；超限或未知 PTS 返回编码错误，断开清理，不将旧输出绑定当前帧。FFmpeg 送帧遇到 EAGAIN 时先取输出再重试，同一输入不重复转换。

依赖替换和实际验收状态见[厂商依赖构建](../development/ffmpeg-vendor-build.md)，必须核对运行时 `ffmpeg_build` 标识，不能用 API 接受或软件回归通过代替硬件实际输出验收。GOP、静止桌面刷新和接收 GPU 路径均未调整，反复变糊仍需真实双机复验。

## 控制端

WebRTC H.264 decoder 输出 I420。默认 `--video-renderer=auto` 下，`KWebRtcRemoteFrameProcessor` 保留引用计数缓冲区的所有权，将只读 Y/U/V 平面交给 D3D11：三张 `R8_UNORM` 纹理按各自 stride 上传，像素着色器执行 BT.601 limited-range 转色。正常路径不分配整帧 BGRA，也不执行整帧 CPU `I420ToARGB()`；这不是采集到编码的 GPU 零拷贝，解码器原有 I420 拷贝及 CPU→GPU 上传仍存在。

`KDecodedVideoFrame` 同时支持 I420 与 BGRA，不暴露 WebRTC/D3D 类型。共享所有权和独立会话 token 保证缓冲区直到消费或丢弃才释放；清理使旧 token 失效，迟到帧不能再次显示。处理线程和 GUI 队列继续只保留最新待处理帧。

`--video-renderer=bgra` 强制使用原有 CPU 转色。I420 专属资源创建失败时，渲染器标记该帧所属会话回退，后续帧在处理线程转换 BGRA；不把转换搬到 GUI 线程，新会话可再次尝试 GPU。整个 D3D 设备失效仍按原有错误路径处理，BGRA 回退不代表无 GPU 的软件显示。格式不变、尺寸不变时复用纹理，清理时释放纹理。

开启 latency trace（包括 `--trace`）时，接收端始终使用 BGRA，以保留现有水印读取、移除及输入反馈语义。因而不能使用该模式证明 I420 GPU 路径的端到端延迟收益；若需观察 GPU 路径，可只开启 session/resource trace。着色器以现有 libyuv 行为为颜色基准，在转换 RGB 后执行双线性过滤，不在本轮增加 HDR 或 BT.709 元数据处理。

`KVideoRenderWidget` 在 GUI/渲染边界显示最新帧，并将画面坐标映射为远端输入坐标。线程安全的 `KLatestDecodedFrameQueue` 保证 GUI 忙时最多保留一张待处理帧，旧帧可被覆盖，但最新帧在当前呈现结束后会再次调度。视频帧携带最近输入序号，用于控制端以单调时钟计算输入发送到画面完成渲染的 round trip。

## 延迟策略与可观测性

- 播放延迟默认使用低延迟配置，不依赖两台机器系统时间计算端到端耗时。
- WebRTC stats 的 `jitterTargetEstimateMs` 是接收端抖动/解码目标估计，不等于画面实际等待时长。接收帧的 `lowLatencyRender=1` 来自 WebRTC `RenderParameters::use_low_latency_rendering`，表示当前帧走低延迟立即渲染路径。
- 高频回调合并旧帧，避免“完整播放过时画面”累积延迟。
- 采集、编码无输出、首帧重推、解码、转换入队和真正呈现均有诊断事件；`render_end` 可拆分 callback-to-convert、convert-to-enqueue、enqueue-to-present 和 callback-to-present。
- 画质配置受能力协商的最大分辨率、FPS 和码率约束。

当前固定采集默认显示器。能力消息包含 monitor list 仅为后续扩展，本阶段没有多显示器切换入口。
## H.264 level 集中约束（2026-10-10）

`codec/h264levelpolicy` 只维护逐行 8-bit Constrained Baseline 的等级限制表和计算，
不依赖 WebRTC/FFmpeg 类型。检查宏块取整、每帧/每秒宏块、单边尺寸、参考帧预算、峰值码率及 CPB。
等级表到 5.2，与本工程捆绑 WebRTC 的最高枚举一致；不支持 1b 或 6.x，不把标准最高等级等同于当前实现能力。

编码和解码工厂共同声明 `42e034`（Constrained Baseline 5.2）、packetization-mode=1、
level-asymmetry-allowed=0，由 WebRTC 进行对称协商；旧 3.1 端仍可协商到 3.1。
工厂不再只检查 H264 名字，而是检查 profile/level/封包模式，并把协商等级传到实例。
目前不利用 max-fs/max-mbps/max-br 等扩展突破基本 level 的限制。

初始化按分辨率、帧率和 maxBitrate 计算最低编码等级；没有 maxBitrate 时预留协议最高码率
与协商等级码率上限两者较小值，并保证不低于起始码率。采用最坏 500ms CPB 预算和一个参考帧，
覆盖 x264 与 AMF 的目标相关缓冲；QSV 在选定等级后按该等级最大码率的250ms预算预留固定缓冲，
并再次通过等级/CPB校验。超出协商范围拒绝初始化，不自动降分辨率/FPS。
选定等级通过公开 `AVCodecContext::level` 传入 QSV/AMF/NVENC/x264，参考帧设为 1；
MF 上游不消费该 level 字段，因此只能依靠实际码流检查拒绝超限输出，不声称 MF 已支持等级设置。
运行中超过初始化等级预算的码率仍锁存错误，需重新初始化/协商。
SetRates 帧率反馈可能因输入节奏波动高于初始化 FPS，不再因此锁存错误：
目标统计值上限为初始化 FPS；非正数及非有限值回退到初始化 FPS，浮点值先限制再转整数。
原始反馈仍保留在诊断中。这不修改编码器配置帧率，也不是实际限帧器；采集节奏仍由采集线程控制。
预算内普通 SetRates 不重开编码器、不主动要求 IDR，真实 SPS/等级检查保持不变。

发送前以及解码前检查 Annex-B SPS：profile、等级、逐行结构、尺寸与参考帧数量。
发送侧同时检查已配置 FPS；接收侧 SPS 检查不推断实际帧率。实际等级可以低于协商上限，
但不能低于覆盖会话码率/缓冲预算所需的最低等级，不强求 SDP 和 SPS 字节相等。
超限/不兼容 SPS 不改写，拒绝并锁存错误；新会话重置状态。
首个可解码图像前必须见到合法 SPS，丢失初始关键帧时可等待后续 SPS 恢复。
这不是完整 H.264 HRD/CPB 合规分析器，不能仅凭 SPS 检查宣称实测码率或网络带宽达标。

软件编码/解码支持能力不代表实时性能保证；Intel 专项结果与双机未验收状态见厂商构建记录。

## QSV 初始化与动态更新兼容修正（2026-10-11）

- FFmpeg 最小补丁只在指定 `qsv_params` 时查询 `MFX_HANDLE_CONFIG_INTERFACE`；
  该字符串配置接口与动态码率使用的 `MFXVideoENCODE_Reset` 不是同一接口。
- 应用明确请求 Constrained Baseline（公开 FFmpeg profile 常量数值578，与 VPL 相同），
  不再请求普通 Baseline 后依赖驱动碰巧输出约束标志。实际 SPS 校验继续保留。
- QSV 保持 CBR；在本 FFmpeg 基线中使用 `FF_COMPLIANCE_UNOFFICIAL` 使 QSV 的
  `NalHrdConformance` 关闭，允许 CBR 动态调整。这不关闭应用的 profile/level/SPS 检查。
  依据 [Intel 动态码率规则](https://intel.github.io/libvpl/latest/appendix/VPL_apnds_c.html#dynamic-bitrate-change)。
- QSV 缓冲区只在初始化时设置，按选定等级最大码率的250ms比特预算预留；未指定等级的独立
  诊断编码使用不低于初始目标的20 Mbps预算。运行中只改平均/最大目标，不缩放此缓冲区。
  这避免 Reset 改变已分配容量；预算并非额外播放等待时间，也不代表实测端到端延迟。
- AMF 的目标相关250ms缓冲、x264 的500ms VBV、MF 限制、GOP、驱动与默认后端选择均不变。
  不重建编码器，不增加额外关键帧请求；错误仍沿现有失败路径报告。
