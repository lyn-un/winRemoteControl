# 构建与测试

## 指针分离测试

2026-10-29提交前复核：普通Release全量构建通过，完整CTest102项中76通过、26显式跳过、
0失败（139.81秒）；真实输入与厂商硬件专项未在本次复跑中启用。历史硬件/双机结果及
手动锁屏触发的ACCESS_LOST见下文和视频链路GPU-CAP-001，不把本次默认测试代替硬件验收。

`wrc_cursor_shape_tests` 不访问桌面、不注入输入。覆盖默认/彩色/单色/MaskedColor、
完整 64 位序号、最大 256 KiB 数据的分块重组、带 padding 的 stride、非法尺寸与热点、
严格 Base64、错通道/版本、缺块/乱序/重复、5 秒超时、替换、reset 及旧序号清理。

`wrc_capture_cursor_shape_tests` 只向采集对象提供合成 DXGI 元数据，不初始化真实
Duplication；检查单色双高度、可见性变化、不回传位置、最新形状覆盖、异常形状回退、
generation 和 shutdown 清理。`wrc_capture_pipeline_tests` 另覆盖 Timeout 时仍转交
指针事件，service 的 200 次更新合并、停止拒绝及重启时旧回调排空。

```powershell
ctest --preset release -R '^wrc_(cursor_shape|capture_cursor_shape|capture_pipeline|protocol|p1_core)_tests$' --output-on-failure
```

`wrc_native_cursor_tests` 使用自有离屏 DIB，不捕获桌面、不安装或移动真实鼠标指针。
精确检查单色四种 AND/XOR 组合、MaskedColor 复制/XOR、半透明预乘颜色、热点和默认/
隐藏/无效输入；另循环创建/销毁 200 次，检查 USER/GDI 对象没有超限累积。彩色像素在
生成原生光标前做反预乘，避免离屏 DrawIconEx 中再次乘透明度变暗；已在本机和远端
Intel 的实际 Windows 环境通过，不以主观截图替代像素检查。

`wrc_session_service_tests` 使用假采集/输入、授权回环连接，覆盖新旧能力、只读观看、
未 Streaming/旧 generation/错误角色/重复事件拒绝、停止重启的连续序号、真实 Qt
5 秒定时器清理和错误重组后恢复。采集事件用会话 generation，Peer 回调用独立的
Peer generation 校验，测试包含被控端监听后接入导致两者不同的情形。

`wrc_webrtc_peer_initialization_tests` 用假 DataChannel 验证水位暂停与恢复：最大形状
32 块、连续替换 50 份仍只缓存最新一份，session 命令优先，部分旧形状不会污染新形状，
停止清空；另检查控制端路由和错误角色/字段拒绝。`wrc_video_render_size_*` 分别以
1、1.25、1.5、2 的缩放比例继续验证物理缓冲和输入映射，
并检查独立指针的物理尺寸/热点不随视频缩小、禁用/清帧后释放。

应用现已发布 `cursorShapeV1`，但这些测试仍不是双机视觉或真实输入验收。默认发送路径
仍为 cpu；与旧端兼容时继续 CPU 合成，双方新版可通过 auto/gpu 显式测试 GPU 链路。

2026-10-27 本阶段普通 Release 全量构建通过；完整 CTest 98 项，74 通过、24 显式
跳过、0 失败（112.88 秒，真实输入显式关闭）。相关 5 项测试另单独通过；远端 Intel
实跑同一指针协议测试退出 0。硬件专项的默认跳过不等于本轮重新完成三后端硬件验收。

接线阶段最新结果：普通 Release 全量构建通过；完整 CTest 99 项，75 通过、24 显式
跳过、0 失败（122.05 秒，真实输入仍显式关闭）。原生指针离屏测试另在远端 Intel
通过；真实桌面指针显示、输入和新 GPU 会话性能仍未纳入该结论。

另完成首轮真实 QSV GPU 双机只读验证：双方同一构建，经配对码比对和明确接入/开始推流；
Intel 被控端强制 gpu/qsv，本机控制。1152×720 切换到原始 1920×1200，请求 30FPS/8Mbps，
可见合成动画期间持续解码，日志确认 actual=gpu、readbackBytes=0、cpuConversion=0、采集池
4槽；未记录捕获/编码错误。同会话停止/恢复、停止后的帧数安静窗口、正常断开通过。
没有注入键鼠；并非固定场景性能对比，也不代表反向 AMD/NVIDIA、跨适配器、主观画质和
30分钟长稳或20次重连已验收。测试程序和动画均已退出，日志保持本地忽略。

反向 NVENC 基础验证另已通过：Intel控制本机，实际采集适配器为NVIDIA，
1280×720切换2560×1440、强制gpu/h264_nvenc、仅观看；20次同会话停止/恢复通过，
另完成1808秒/89样本长稳。新版实际HDR复验确认inputFormat=10、hdrGpu=1、
读回/CPU转色0；约56秒采集1675帧/错误0，原画尺寸持续接收。这些不代表主观色彩验收。
AMF auto实际双机已确认跨卡CPU回退（111810→103674），90样本持续收帧观察结束，
首末样本跨度1809.44秒，最终采集/编码错误0。脚本最后读宿主状态因driver session空闲
过期报错退出1，保留失败；该宿主此前30分钟没有被脚本轮询，不能解释成视频断流。
末段资源仅21样本/418.86秒，不能当全程连续峰值。物理键盘及100%/150%DPI用户明确暂缓。
强制AMF/gpu另以可见本机回环验证：capture_pipeline_error明确cross_adapter，编码输出0、
接收帧0，经原错误链断开；没有静默CPU回退。不是AMF同卡真实桌面验收。

## GPU 硬件帧接入专项（发送链路显式启用）

发送链路 CPU/GPU 对照工具为 `wrc_gpu_encoding_benchmark`，只使用合成 GPU 图像，
不属于默认 CTest。预热/采样、解码校验、timestamp 边界和当前验证状态见
[合成 GPU 编码链路基准](gpu-encoding-benchmark.md)。

`wrc_gpu_nv12_tests` 和 `wrc_gpu_nv12_warp_tests` 验证独立的 BGRA→NV12 GPU
模块：BT.601 limited-range、双线性缩放、2×2 UV 平均、奇数源尺寸、输入 stride
padding、输出对齐填充及无效参数。对比独立标量参考，Y/UV 最大误差门限为 3；
1:1 另与现有 `SWS_FAST_BILINEAR` BGRA→I420 实际输出比较，Y/UV 门限也为 3，
并核对本构建 swscale 默认矩阵和 range。缩放情况的 swscaleMaxY/UV=-1 表示未用
swscale 作逐像素参考；这部分对比显式标量采样算法。尚未覆盖真实桌面清晰度或完整 HDR 串联。

`wrc_gpu_hardware_frame_probe h264_qsv|h264_amf|h264_nvenc [--rgb]` 仅在
`WRC_RUN_GPU_FRAME_TEST=1` 时运行，未启用或没有相应厂商显卡时返回 77；有显卡但
初始化失败则失败，不拿软件编码替代。默认在 GPU 清空 Y/UV 平面生成 NV12；`--rgb`
在 GPU 生成 BGRA 并经新转换器输出 NV12。两者都编码 1280×720、30 FPS、90 帧，
按 PTS 连续软件解码并验证尺寸、帧数、重复 PTS 与采样 YUV，最大误差门限 6
（包含有损 H.264）。输入无 CPU 像素上传或 GPU 读回，软件解码只用于验证输出。

探针复用 `KD3D11HardwareFrames`：8 张独立 NV12 纹理，D3D11 直接输入 AMF/NVENC，
QSV 使用 D3D11 派生设备与 DIRECT 映射。先验证池满背压、编码器额外 AVFrame 引用
阻止提前重用、池 reset 后旧帧存活及 generation 更换，再运行实际编码。
这些测试不证明任意驱动的异步实现无缺陷、跨显卡共享或长时间无资源增长。
探针使用独立低延迟编码配置，不替代真实应用的 level、动态码率及主动 IDR 回归。

```powershell
cmake --build --preset release --target wrc_gpu_nv12_tests wrc_gpu_hardware_frame_probe
ctest --preset release -R '^wrc_gpu_nv12' -V
$env:WRC_RUN_GPU_FRAME_TEST = '1'
ctest --preset release -R '^wrc_(qsv|amf|nvenc)_gpu_.*frame_tests$' -V
```

`wrc_webrtc_gpu_frame_tests h264_qsv|h264_amf|h264_nvenc` 同样由上述环境变量显式启用。
它生成 GPU BGRA，通过真实自定义 WebRTC 编码适配器和解码适配器验证 320×240、
30 FPS 媒体时间的 96 次提交，要求最先 90 帧完整、RTP 元数据无重复、采样亮度误差≤6。
额外 6 帧用于覆盖异步尾部，保留现有 teardown 丢弃挂起回调语义，不声称 Release 排空全部尾帧。
第 45 帧主动请求关键帧；正式码率准确性/不重建回归仍由原有码率测试覆盖。
固定 gpu 模式下任何 ToI420 请求都会锁定失败，因此成功不能悄悄来自 CPU 格式回退。

默认 `wrc_webrtc_native_fallback_tests` 用 WARP 验证 auto 的读回/I420 兼容、原生缩放
不立即读回，以及 gpu 拒绝隐式转换；`wrc_video_frame_path_tests` 验证 generation、
回退锁定和运行期失败。`wrc_native_capture_tests` 不初始化 DXGI duplication，仅用
合成 BGRA/FP16 纹理验证 4 槽采集池背压、迟到释放、换代及 HDR GPU 输出。
该专项另连续20次交替64×48/80×50，保留前一帧引用检查尺寸重建及shutdown后读回仍有效；
以不支持的合成RGBA格式分别验证auto初始化固定CPU回退、已经选GPU后的失败锁存及强制gpu
失败，再检查新generation可以恢复。已构建/专项通过，不采集桌面；这是错误边界测试，
不是主动移除真实显卡设备或驱动重置验收。

以上合成专项不采集桌面、不注入输入。指针能力协商与实验采集/编码均已接入，
正常程序默认CPU，auto/gpu需显式选择；真实运行覆盖单独记录，不能由合成测试推断。

配置生命周期专项：`wrc_application_paths_tests` 验证临时目录检查及文件保留，
Python SDK 单测验证启动前拒绝复用；`wrc_device_identity_tests` 使用独立子进程建立、
重载签名信任库并验证篡改拒绝，最后仅清理本次测试身份。该测试依赖当前用户 Windows CNG。
`wrc_session_service_tests` 使用 fake peer/隐私适配器模拟结束应答后通道先关闭，覆盖
隐私启用、禁用、恢复失败及旧 generation 状态，确认本地恢复仍可观察且停止时不广播状态。

进程级专项无需采集或注入输入：

```powershell
$env:WRC_RUN_E2E = '1'
$env:WRC_EXECUTABLE = (Resolve-Path 'build/Release/winRemoteControl.exe')
python -m unittest discover -s automation/tests -p 'test_profile_lifecycle.py' -v
```

此专项实际重启配对进程，验证持久身份与信任记录、双方主动断开，以及临时配置被拒绝后
文件哈希保持不变。默认不运行真实遮罩、关屏或锁屏；其他显式测试门禁仍独立控制。

## NVENC 专项

硬件专项由 `WRC_RUN_VENDOR_BITRATE_TEST=1` 显式启用，默认CTest跳过。
运行 `wrc_vendor_bitrate_output_tests h264_nvenc` 检查200次更新和1→6→1 Mbps严格对照；
`--levels`、`--startup-rates` 检查含1440p30的SPS/解码与低码率启动；`--resources`
检查1080p60节拍及20次生命周期；`--frequent-rates` 比较30秒媒体时间内每秒3/4 Mbps
切换和固定3 Mbps，记录IDR、100ms/1s滑动媒体码率峰值及提交耗时。后者不是实时网络
吞吐测试，峰值也不是网卡瞬时流量。NVENC重配置允许额外IDR，实例不得重建。

### NVENC 重配置组合对照

补丁版依赖增加两个独立AVOption，库默认均为1；应用检测到选项后默认设置0/0，
旧库缺少选项则记录原因并保持库默认。硬件测试程序接受
`--nvenc-reconfig-reset=0|1 --nvenc-reconfig-idr=0|1`，两项必须一起指定，
只用于`h264_nvenc`的严格码率、`--frequent-rates`或`--keyframe-policy`测试。
重复参数、非法值、其他后端或与资源/等级测试混用返回2。主程序没有新增命令行参数。

```powershell
$env:PATH = (Resolve-Path build/Release).Path + ';' + $env:PATH
$env:WRC_RUN_VENDOR_BITRATE_TEST = '1'
build/cmake-release/tests/Release/wrc_vendor_bitrate_output_tests.exe h264_nvenc --keyframe-policy --nvenc-reconfig-reset=0 --nvenc-reconfig-idr=0
pwsh -File tools/ffmpeg/benchmark-nvenc-reconfiguration.ps1
```

组合脚本使用同一套DLL，默认各重复三次1/1、1/0、0/0的严格码率、高频更新和关键帧测试，
结果及输入哈希写入`build/nvenc-reconfiguration/`。不采集桌面、不注入输入。
关键帧测试在第47、113帧更新码率，第179帧显式请求IDR；解析Annex-B NAL及PTS，
检查连续900帧解码与实例不重建。默认CTest另登记四种组合的关键帧测试，硬件门禁关闭时跳过。
当前FFmpeg开启PTD，SDK规定reset本身也强制IDR，因此1/1、1/0、0/1均预期在更新点产生IDR，
0/0预期没有这两个附加IDR；第179帧显式请求及后续周期IDR在四组中都必须保留。
严格码率沿用1→6→1 Mbps与固定对照门槛；新增阶段切换后100ms/1秒输出统计，
不会用丢弃过渡帧后的稳定均值替代响应时间。NVENC测试将重配置错误日志视为失败。
AVOption成功只证明选项已设置，GPU实效仍须看码率、IDR及解码结果。

实现和硬件验证状态见[厂商构建记录](ffmpeg-vendor-build.md#nvenc-重配置选项)。

2026-10-16：普通Release全量构建、默认CTest（67通过/11跳过/0失败）、NVIDIA严格码率、
六组启动/SPS及auto探针通过；双机1440p持续600秒及独立停止/重连验证完成。QSV严格
回归和反向短会话、AMF六组启动回归通过。频繁IDR画质代价、每次生命周期约2个句柄
增长及旧断开状态错误仍保留，详见[厂商验收记录](ffmpeg-vendor-build.md)。

## 默认厂商编码器选择

`ctest --preset release -R '^wrc_video_encoder_selection_tests$'` 验证 Intel/AMD、
DXGI 顺序、同厂多卡身份、软件适配器、NVIDIA-only、空列表、硬件失败后其他硬件/
x264 的顺序，以及显式选择失败不回退。使用模拟初始化结果，不冒充硬件验收。

构建资源诊断目标后，可在运行库已部署的环境执行
`wrc_auto_encoder_probe [h264_qsv|h264_amf|h264_nvenc|libx264]`，参数是期望结果断言，不会强制选择。
该工具始终使用默认 auto，分别以320×240、1280×720、1920×1080合成图初始化/编码/
排空/解码，每组120帧，1→6→1 Mbps请求，检查唯一PTS、帧数、尺寸及实例不重建。
不采集桌面、不注入输入。它只验证选择及编码连续性，不设码率响应/PSNR门槛，
不能替代原有严格动态码率测试。选择日志写入探针同级 auto-encoder-logs。

2026-10-15 验证：本机 auto 选择 AMF、Intel 端 auto 选择 QSV，均完成上述三组各120帧
的连续解码与唯一PTS检查。普通 Release 全量构建通过；完整 CTest 67通过、8显式跳过、
0失败（121.31秒），硬件专项不计入该默认通过数。远端显式 QSV 严格码率专项退出0；
本机显式 AMF 严格专项仍退出1，低码率偏高与历史结果一致，不能记为全部硬件验收通过。
随后已完成下述真实短程视频会话；长期稳定性与主观画质仍需单独验证。

2026-10-15 双向 LAN 实测：两台真实主机通过隔离配置、匹配 TLS 配对码、正常审批及
viewScreen 权限连接；被控端显示可见运动窗口，未注入键鼠。AMD 控制 Intel 实际选择
QSV，1152×720→1920×1200持续出帧，首次停止计数649，重连后停止计数144。
Intel 控制 AMD 实际选择 AMF，1280×720→2560×1440持续出帧，首次停止计数1411，
重连后停止计数305；AMD当时实际走HDR FP16 GPU转SDR路径。两方向采集、转换与编码
watch均未出现非零errors；停止回到Connected、计数安静，断开控制端回Idle。

这仅支持视频路径、尺寸切换及一次重连的功能验证。QSV首轮接收统计丢包5个、NACK4、
PLI1，解码丢帧0；AMF首轮丢包0、PLI1、解码丢帧0。被控端断开后仍报既有
send_failed/session_channel_closed，脚本的passed字段只表示视频检查通过，不能称为
全程无错误。前置分步脚本曾审批超时、在视频窗口创建前设置画质被拒绝，调整编排后才
运行成功场景；原日志保留。临时测试应用均正常发起退出，未覆盖长期、键鼠及主观画质验收。

## 接收端物理像素回归

`ctest --preset release -R '^wrc_video_render_size_' -V` 创建短暂的合成画面原生
Qt/D3D11 窗口，不采集桌面、不注入输入。四个进程分别设置 QT_SCALE_FACTOR 为
1、1.25、1.5、2，检查实际 back buffer、客户区、视口、逻辑坐标映射、奇数窗口
尺寸、最小化恢复和最大化恢复，覆盖 BGRA/I420。测试运行串行且有超时。
这是 Qt 缩放因子的原生回归，不等同于实际切换 Windows DPI 或跨显示器验收。
2026-10-13 本机四组通过（9.79 秒），Intel 端四组均退出 0。Intel 默认 DPR 为
1.25，QT_SCALE_FACTOR 会与系统比例相乘，四组实际 DPR 是 1.25/1.5625/1.875/2.5，
并非更改了 Windows 设置。本机普通 Release 全量构建通过，完整 CTest 为
66 通过、8 显式跳过、0 失败（123.86 秒）。此阶段尚未进行双机画质复验，后续记录见下文。

同日补强测试：不再由断言函数主动调用 render，直接验证静止帧的 resize/恢复重绘；
增加四角映射、最小化保持缓冲区、全屏恢复和模拟过期缓冲区后的 DPR 通知恢复。
本机四组再次通过（11.77 秒）。模拟通知只验证事件处理路径，不冒充真实跨屏变化。

2026-10-13 Intel 控制 AMD 原画复验：用户确认清晰度“确实好了一些”。日志中
DPR=1.25，客户区与缓冲区在窗口变化后均为1600×900、1728×973、1920×1150；
原画解码为2560×1440，实际使用I420 GPU路径。属于真实会话尺寸及主观改善证据，
不是同内容量化画质A/B，不宣称所有模糊消除；反方向修复后回归及真实跨屏仍未覆盖。

## 视频流水线汇总日志回归（2026-10-11）

新增诊断用例复用 `wrc_webrtc_h264_encoder_rate_control_tests`：验证没有新帧时仍输出
周期快照、最终计数、成功/跳过/错误分离、最近有效尺寸、累计/最大耗时，以及仅统计
成功交付给 WebRTC 的编码字节和关键帧。该用例不捕获桌面、不注入输入。
普通 Release 全量构建通过；完整 CTest 58 通过、8 显式跳过、0 失败，99.65 秒，
编码/诊断专项 17.45 秒。记录位于 `build/video-pipeline-diagnostics-build.log` 和
`build/video-pipeline-diagnostics-ctest.log`。硬件和真实输入测试未启用；双机新增日志仍待复测。

本机 `build/Release` 与 Intel 端 `D:\Qt_Projects\winRemoteCtrl\Release` 的主程序已同步，
SHA-256：`BB2CE0EAB357F73D49BB12918508B7024925A493E1EE0A6D7526FF686EA0FB2B`。
远端旧主程序保存在源码目录下
`build/intel-diagnostics/winRemoteControl-before-pipeline-diagnostics-20261011.exe`；
未替换依赖 DLL、设置或日志，未自动启动远控会话。

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

### 接收端 I420 渲染验证

`wrc_d3d_i420_tests` 使用本机 D3D11 硬件离屏目标，比较生产 I420 着色器和 libyuv BGRA 路径；覆盖灰阶、彩条、随机像素、奇数尺寸、padding stride、放大/缩小和资源重建，不捕获桌面或注入输入。无可用硬件设备时测试报错，不静默记为通过。`wrc_webrtc_remote_frame_processor_tests` 验证共享所有权、回退及会话重置；`wrc_webrtc_remote_frame_trace_tests` 在独立进程验证 latency trace 强制 BGRA。

同一个离屏可执行文件提供可重复 A/B 基准，参数依次为宽、高、目标 FPS、`i420|bgra`、测量秒数、预热秒数：

```powershell
$env:PATH = "$PWD/build/Release;$env:PATH"
build/cmake-release/tests/Release/wrc_d3d_i420_tests.exe --benchmark 1920 1080 60 i420 60 10
build/cmake-release/tests/Release/wrc_d3d_i420_tests.exe --benchmark 1920 1080 60 bgra 60 10
# 完整矩阵：约 28 分钟，三轮交替顺序，本地 JSON 保存到 build/diagnostics/reports
powershell -NoProfile -ExecutionPolicy Bypass -File tools/measure-receiver-render.ps1
```

上述执行策略参数只作用于本次 PowerShell 进程，不修改用户或机器的持久执行策略。脚本默认将报告保存在已忽略的 `build/diagnostics/reports`；中断时保留已完成案例，并保持 `completed=false`。

分别对 1920×1080、2560×1440 和 30/60 FPS 各运行三轮，两种模式串行交替，测试期间避免构建及其他重负载。输出 JSON 包含 CPU 时间、准备/上传提交/离屏 Draw 调用 P50/P95、GPU 查询时间、有效上传字节、错过节拍数和预热后/结束时资源快照。基准使用相同静态合成 I420，不包含采集、网络、解码、Qt 调度或交换链 Present；错过节拍不是远程视频丢帧率，资源快照不是峰值，不能将结果当作双机端到端性能。

正式交付仍须复验原生窗口、真实双机视频、颜色、缩放和重连。个体机器的性能结果保存在本地，不把未实测的 FPS 或延迟当成已达成指标。

### x264 实际码率回归

`ctest --preset release -R '^wrc_webrtc_h264_encoder_rate_control_tests$' -V` 同时运行原有 200 次速率变更检查和真实编码用例。后者强制软件编码，固定 1280×720、30 FPS、种子 `0x6d2b79f5` 的移动灰度纹理，同一实例依次请求 1/6/1 Mbps，每阶段 300 帧，前 60 帧不计入测量；另运行固定 1 和固定 6 Mbps 的各 900 帧对照。全部输出逐帧软件解码，检查尺寸和帧数，并比较亮度 PSNR。它不捕获桌面、不注入输入，不要求硬件编码器。

输出 `requestedKbps` 是请求值，`measuredKbps` 按实际编码字节和输入媒体时间计算，不按测试墙钟速度计算。高阶段须至少为首个低阶段的两倍，降回后不超过高阶段的 60%；与相应固定码率对照偏差不超过 30%，高阶段 PSNR 不低于两侧低阶段。覆盖默认码率、重复/零/负值、极小正值、VBV 整数上限及上下文/时间戳连续性。VBV 输出存在内容依赖，不要求精确填满目标。

旧实现已在相同合成用例中复现升码率不响应，修复后通过；这是码率控制验证，不是双机模糊根因确认，也不是整个程序的画质或性能保证。个人数值和原始日志保留在根目录忽略的 `performance-baseline.md` 与 `build/diagnostics/x264-rate-control/`。

## Release 构建

QSV/AMF/NVENC 依赖构建、AMF 补丁和部署状态见[厂商后端构建](ffmpeg-vendor-build.md)，当前仍在实施和验证中。
[MF 实验记录](ffmpeg-mf-build.md)仅保留历史证据；旧 MF 补丁及 helper 已退出活动构建。
`wrc_mf_bitrate_output_tests` 默认跳过，仅在 `WRC_RUN_MF_BITRATE_TEST=1` 时作为限制复现工具执行，
其动态码率断言失败不能用来宣称新后端失败或通过，不再是 MF 动态更新交付门槛。不捕获桌面或注入输入。

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

### 厂商 H.264 显式测试

默认 CTest 跳过 QSV/AMF 硬件测试。单独启用：

```powershell
$env:WRC_RUN_VENDOR_BITRATE_TEST = '1'
ctest --preset release -R 'wrc_(qsv|amf)_bitrate_output_tests' -V
Remove-Item Env:WRC_RUN_VENDOR_BITRATE_TEST
```

无匹配厂商 GPU 返回 77；发现 GPU 但初始化、编码或验收失败均算失败，不切换 x264。
日志必须确认 `FFmpeg=` 构建标识，防止可执行文件旁的旧 DLL 优先于 PATH 被加载。
测试使用固定种子 720p30 动画，同实例 1→6→1 Mbps 和两组固定对照，按 PTS 连续解码、
验证帧数与 baseline profile，报告关键帧位置、码率目标偏差和亮度 PSNR。阈值见厂商接入规划。
解码器启用 `AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS`，按“帧基础 QP + 宏块 delta”统计稳定段
实际亮度 QP 的最小/最大/平均值和 QP=51 占比；没有 side data 时报告未知值，不把请求参数当作实测 QP。
缺少匹配 GPU 时还会确认显式后端初始化失败且不回退，再将硬件验收标为跳过。
WebRTC 纯逻辑测试另覆盖延迟出包、多帧输出的元数据对应以及释放后迟到输出拒绝。

`build/cmake-release/tests/Release/wrc_vendor_bitrate_output_tests.exe h264_amf --resources`
在同一显式环境开关下运行 1080p60 的 600 帧节拍测试和 20 次交替尺寸启动/释放观测。
直接运行时把匹配的 FFmpeg `bin` 和 Qt `bin` 放入当前进程 PATH。它记录编码调用分位数、
进程 CPU 时间、句柄和私有字节；CPU 包含合成帧生成，结果不是双机 FPS 或端到端延迟。
资源观测成功仅表示编码/排空和 PTS 检查通过，不自动把资源增长判为可接受。

### 手动流程

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
## H.264 level 验证

默认 `wrc_webrtc_h264_encoder_rate_control_tests` 覆盖集中等级计算的分辨率/FPS、码率、
CPB、DPB、宏块取整及整数边界；旧 3.1 与新 5.2 offer/answer 双向组合；不支持的 profile、
封包模式与等级；实际软件编码→SPS 检查→解码；超限码流拒绝和等待关键帧恢复。
真实软件测试包括 720p60、1080p60、4K60，各三帧，只证明功能，不衡量实时帧率。

硬件等级检查独立于严格码率测试，仍须显式启用：

```powershell
$env:WRC_RUN_VENDOR_BITRATE_TEST = '1'
ctest --preset release -R '^wrc_(amf|qsv)_level_tests$' -V
```

每后端验证 720p30/1080p30/1080p60 及问题场景 1152×720p30/1920×1200p30，各12帧、最终排空、实际 SPS 上限和连续解码。
附加 `--startup-rates` 专项使用相同尺寸，每组120帧，初始300 kbps、提交一帧后降至235 kbps，再于第40/80帧切换至6000/235 kbps，覆盖低码率启动及后续预算增长；仍须通过 SPS 和完整解码检查。它不代替高细节严格码率测试，均要求显式设置 `WRC_RUN_VENDOR_BITRATE_TEST=1`。测试程序可附加 `--verbose` 输出 FFmpeg 参数查询/重配置诊断；此参数不属于主程序。
无对应 GPU 跳过，有 GPU 的打开/配置/编码/检查失败记失败。默认不启用。
通过 level 专项不能替代原 `1→6→1 Mbps` 严格门槛或双机兼容性复验。

### 另一台机器无开发环境时的复验

不必在目标机器安装 CMake、Qt SDK 或重新构建 FFmpeg。使用本次完整 Release 部署文件，
额外将 `build/cmake-release/tests/Release/wrc_vendor_bitrate_output_tests.exe` 放到
目标程序目录，与 `Qt6Core.dll` 和本次 FFmpeg DLL 同级。不要复制开发机的 `security`、
可信设备、设置或日志，也不要覆盖目标机器原有身份数据；不要只更新单个 FFmpeg DLL。

在目标程序目录打开 PowerShell，Intel GPU 机器执行：

```powershell
$env:WRC_RUN_VENDOR_BITRATE_TEST = '1'
try {
    & .\wrc_vendor_bitrate_output_tests.exe h264_qsv --levels *> qsv-level.log
    $levelExit = $LASTEXITCODE
    & .\wrc_vendor_bitrate_output_tests.exe h264_qsv *> qsv-bitrate.log
    $bitrateExit = $LASTEXITCODE
    "levelExit=$levelExit bitrateExit=$bitrateExit"
} finally {
    Remove-Item Env:WRC_RUN_VENDOR_BITRATE_TEST -ErrorAction SilentlyContinue
}
```

AMD GPU 机器将参数换为 `h264_amf`，日志另取 `amf-*.log`；不要按 CPU 品牌判定 GPU。
返回 0 表示该项通过，77 表示无匹配硬件而跳过，其他非零表示失败。
记录 GPU/驱动、两项退出码和完整输出；加载器缺 DLL 也属于部署失败，不算硬件跳过。
上述测试只生成合成画面，不采集桌面、不注入输入，也不建立远程连接。

等级和码率测试分别通过后，才继续人工双机验证。双方更新本次程序，被控端显式使用
`--diagnostic-video-encoder=h264_qsv` 或 `h264_amf`，以 `libx264` 作同设置对照。
为采集 level/码率诊断使用 `--latency-trace`（不是 `--trace`，不附加输入追踪）；
此开关会使用 BGRA 显示，因此所有对照均保持相同开关，不能借此评价 I420 GPU 渲染性能。
窗口切换、滚动、视频和静止恢复分别标记时间，保存双方日志；再检查断开重连及分辨率变化。

2026-10-11 冻结定位补充：新增发送端 `--session-trace` 编码快照，详见诊断文档。
只定位卡住阶段时不必启用 latency trace。`wrc_webrtc_h264_encoder_rate_control_tests`
增加独立线程在无编码进展时持续记录、累计计数、重复错误限频、Release 最终快照，
以及真实码率超出等级预算的拒绝日志回归。

2026-10-11 帧率反馈冻结修复：同一 libx264 适配实例依次接收 30/31/39/28/13/30 FPS，
再覆盖零、负值、NaN、无穷、极大有限值和小数。保持实际输入时间戳约30 FPS，
每阶段验证继续输出、连续解码及尺寸、PTS 不重置、码率请求更新和不主动要求关键帧。
新增用例在修复前失败（`build/fps-feedback-old-test.log`），复现反馈越界后永久拒绝编码。
该测试不代替 Intel QSV 双机桌面复验，也不证明实际送帧节奏没有瞬时波动。

本次普通 Release 构建及诊断专项通过；完整 CTest 66项中58通过、8显式跳过、0失败，
耗时92.00秒（`build/encoder-diagnostics-ctest.log`）。`git diff --check`通过。
本机 `build/Release` 已更新。Intel远端MCP曾返回HTTP502；恢复Tailscale连接后，
2026-10-11已将诊断主程序部署到 `D:\Qt_Projects\winRemoteCtrl\Release`，
本地与远端SHA-256均为 `40420477A202110DA32BB6537A5A39AD0A1E82A94E057877FE53E4C794415481`。
替换前程序保存在远端 `build/intel-diagnostics/winRemoteControl-before-diagnostics-20261011-152819.exe`，
未覆盖DLL、设置、配对或日志；尚待用户启动会话复现冻结，未自动开启桌面采集。

随后用户会话日志确认：SetRates 出现31 FPS 后锁存 levelError，回落到28/13 FPS仍不恢复；
编码调用已返回且诊断 phase=idle，不是 FFmpeg 调用仍在阻塞。
帧率反馈修复后普通 Release 全量构建通过，完整 CTest 58通过、8显式跳过、0失败，
耗时93.69秒，其中编码速率回归13.85秒（`build/fps-feedback-ctest.log`）。
本机和Intel远端上述运行目录主程序均已更新，SHA-256为
`C8169CE0ABC5E224881BD56AE5C11E92542F6196F4A7790D201AE58BD8B4FF51`。
远端旧程序备份为 `build/intel-diagnostics/winRemoteControl-before-fps-feedback-20261011.exe`。
未重编/替换FFmpeg、未自动启动远控或输入；修复后QSV双机画面持续性仍待用户复验。

## 跨适配器共享入口探针

`wrc_gpu_cross_adapter_probe` 通过 `WRC_RUN_GPU_FRAME_TEST=1` 显式启用，默认返回77。
它只使用前两个不同LUID的硬件适配器创建64×48合成BGRA/NV12纹理，不采集桌面或注入输入；
少于两个合适适配器时显式跳过。每个源适配器分别跑同卡第二设备对照与另一适配器，双向共8例。

入口限定为当前D3D11 render-target纹理的NT共享句柄/keyed mutex；先检查创建、导出和打开，
打开后经目标设备复制、诊断读回逐字节比对。`validProbe=true`表示探针结果可用，不表示
跨卡支持；必须逐例看 `opened`、`verifiedBytes`、失败阶段和HRESULT。
同卡BGRA基线失败或打开后数据不一致会使探针失败，不能归为“硬件不支持”后跳过。
NV12共享纹理创建失败与跨卡打开失败分开报告，不能推断所有跨卡方案都不可行。

同步遵守[CreateSharedHandle](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle)、
[OpenSharedResource1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1)及
[AcquireSync](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync)契约：
NT句柄由RAII关闭，mutex只接受S_OK，等待上限500ms，不把WAIT_TIMEOUT等非负值当作成功。
结果不证明FFmpeg后端可直接编码该资源，也不覆盖D3D12跨卡复制；不改变现有CPU回退策略。
该探针已构建并显式运行通过（0.88秒）：本机同卡BGRA/NV12逐字节对照通过，
AMD/NVIDIA双向跨卡均在打开目标资源阶段返回0x80070057，当前入口不通；不推断所有跨卡
实现都不可能。新增后的普通Release全量构建通过，完整CTest101项75通过、26显式跳过、
0失败（115.21秒）；该硬件探针另显式执行通过，不把默认跳过当作硬件通过。

## DXGI 帧释放边界

DXGI帧释放专项`wrc_dxgi_frame_lifetime_tests`使用fake duplication，不创建显卡设备或采集桌面。
覆盖200次成功获取/释放、连续200次纹理接口查询失败仍平衡释放、超时不释放、
释放失败立即报错、处理/释放双错误保留及后续获取
失败的前次释放诊断。分离指针模式会容忍形状读取失败并回退箭头，因此原始形状错误传播
用例显式采用旧合成模式；不为断言通过改变正常兼容行为。该专项已构建并运行通过。

## 真实输入测试范围

`wrc_windows_input_double_click_test` 仍需显式设置 `WRC_RUN_INPUT_E2E=1`。
它只对自身创建的空白窗口发送测试双击；每次注入前检查窗口归属当前进程、位于前台，
且目标点命中该窗口。失焦或被遮挡时拒绝继续，不把焦点抢回后盲目重试。
新增保护已构建并实跑：本次窗口未取得前台，保护拒绝注入，按钮/双击计数均为0，
因此不能记实际双击功能通过。需正常激活测试窗口再验，不能去掉保护凑通过。
该平台注入测试不等同于双机传输、键盘、滚轮或指针形状验收。

测试可执行文件新增可选 `--wait-for-focus`（不是主程序参数）：最多等待30秒供操作者
正常激活自有空白窗口，期间不注入。超时仍由原有前台/命中保护拒绝，不强抢焦点；
默认CTest行为不变。此可选等待模式已构建并直接运行退出0，源测试全部断言通过，
窗口随后关闭；无需额外点击或放宽保护。此前CTest失焦拒绝记录仍保留，不能以这次通过
推断所有启动上下文都能自动取得前台，也不代表双机输入链路通过。

`wrc_remote_input_fixture` 是独立的可见、只接收事件的空白窗口，不加入默认CTest。
仅在 `WRC_RUN_INPUT_E2E=1` 时打开，否则返回77；35分钟自动关闭。五个水平区域分别
显示箭头、文本、水平调整大小、忙碌及隐藏指针。100ms合成重绘仅用来保持明确画面变化。
它不注入输入、不安装全局hook、不保存字符或键值，仅输出点击/滚轮/按键/字符数量。
双机输入测试必须另经用户授权、正常配对并授权输入，确认远端目标点处于该窗口客户区且
前台仍为该窗口后才发送。键盘只用无个人内容的测试字符；失焦、窗口关闭或遮挡立即停止。
计数用于确认实际到达远端，不能把本地发送成功当作输入成功。目前该夹具已编译、部署Intel
并核对哈希，两机未启用保护均返回77；双机实跑尚未完成。新增后普通Release全量构建、
完整CTest101项（75通过/26显式跳过/0失败，119.20秒）及diff检查通过；100%/150%
系统DPI真机检查不能由Qt离屏比例测试替代。

随后真实双机基础输入：AMD控制Intel，正常配对仅viewScreen/inputControl，Intel显示独立
空白夹具；经本机原生视频窗口点击、滚轮后远端夹具计数增长，未操作其他软件。
文本、水平调整大小、忙碌及隐藏区域已观察到对应光标变化，返回箭头区域恢复。
Intel夹具实测DPI120（125%），不是100%/150%矩阵完成；截图中另有带光晕箭头，来源未
隔离，不以此宣称所有外部指针叠加已消除。键盘自动注入因既有LLKHF_INJECTED过滤被忽略，
未移除保护；物理键盘待单独确认，不把自动键未到达直接判为产品键盘失败。

## HDR GPU 转色测试

GPU 采集设备缓存另有 `wrc_capture_device_probe_tests`，使用
`WRC_RUN_GPU_FRAME_TEST=1` 显式启用。它只创建设备和合成纹理，不初始化 DXGI duplication、
不采集桌面、不注入输入；检查先预览再 GPU 的 VIDEO_SUPPORT 升级、旧设备所有权及后续缓存复用。
默认 CTest 未启用时返回77。该探针已构建并在本机、Intel端显式运行通过；不替代真实采集。

`wrc_hdr_gpu_bgra_tests` 在硬件 D3D11 上验证离屏转换，`wrc_hdr_gpu_bgra_warp_tests` 使用 WARP。
两者不采集桌面、不注入输入；覆盖全部 FP16 位模式、CPU 原公式与独立参考对照、最大 1 色阶
误差、Alpha、奇数/窄尺寸、输入 stride、白电平变更、同尺寸资源复用及 20 次 reset。
安装 Debug Layer 时检查 warning/error；设备创建不可用返回 77，WARP 通过不代表硬件性能通过。

补充用例直接向生产采集读回函数提供合成纹理，不调用桌面 duplication 初始化：检查
auto/cpu、SDR 绕过、低功能级设备导致的真实着色器初始化失败与锁存 CPU 回退、停止清理，
以及彩色/单色指针从干净桌面缓存重绘。跨设备重建、非法参数后的恢复和每轮 reset 后
设备 COM 引用数回归也纳入检查；COM 引用计数不等同于完整 GPU 显存泄漏证明。

`wrc_hdr_capture_fault_tests` 单独编译采集源文件，只有该目标定义
`WRC_HDR_FAULT_TESTS`，正式采集库不包含注入分支。分别模拟 shader、SRV、RTV
创建失败和设备移除，验证一次性回退、设备失效不回退以及新实例恢复 GPU。
`wrc_hdr_resource_cycle_tests` 在硬件上执行 120 次 1080p/1440p 交替创建/读回/释放，
每 10 次输出进程私有内存和可用时的 DXGI 本地显存用量；以第 20 次为预热基准，
后续私有内存增长不得超过 32 MiB、本地显存增长不得超过 64 MiB，允许驱动缓存。
它只证明该循环中没有超限增长，不证明任意长会话、其他驱动或真实桌面生命周期无泄漏。

```powershell
ctest --preset release -R '^wrc_hdr_' -V
```

`wrc_hdr_capture_benchmark` 是显式运行的合成基准，不属于默认 CTest。程序参数依次为
`cpu|gpu|desktop|d2d width height fps repetition`，固定预热 10 秒、采样约 60 秒。运行前将 Qt bin 加入 PATH。
`gpu`保留旧GPU裁剪基线，`desktop`使用产品默认的新肩部映射，CPU为旧标量裁剪。
`d2d` 是未通过桌面颜色门槛的隔离候选，不是产品路径。2026-10-31 前的历史结果使用 5/30 秒，
不能将不同采样时长的 CPU 总量直接比较；当前 JSON 明确记录时长。
输出 JSON 包含实际设备/驱动、帧数、错过截止时间、CPU 时间、帧耗时分位数、Map 等待、
CPU 转色/行复制时间和延迟读取的 GPU timestamp 数据。末尾 8 个 query 不等待回收，另列不可用结果。
CPU 路径包含真实旧标量转换和 FP16 staging；GPU 路径包含输入复制、离屏绘制和 BGRA8 读回。
以每帧 CPU 时间而不是不同帧数下的 CPU 总时间比较收益。

```powershell
.\build\cmake-release\Release\wrc_hdr_capture_benchmark.exe cpu 2560 1440 30 1
.\build\cmake-release\Release\wrc_hdr_capture_benchmark.exe gpu 2560 1440 30 1
```

完整矩阵为 1080p/1440p、30/60 FPS、CPU/GPU、各 3 次，串行运行，避免其他编译或测试干扰。
新肩部映射的矩阵将比较模式换为 `gpu`/`desktop`，同样为24组，其余采样条件不变。
上述测试不证明真实 HDR 桌面帧率、双机画质或输入延迟；这些必须经用户启动/接受的会话验证。
Direct2D 候选的停止条件、结果与重现见 [HDR 色调映射评估](hdr-tone-mapping-evaluation.md)。

2026-10-12 经用户授权，另以现有自动化驱动运行真实 HDR 双机基础 A/B：独立配置，
正常配对并仅授予 viewScreen，AMD 被控端、Intel 控制端，libx264，2560×1440，
请求 30 FPS/8 Mbps；日志确认 auto 实走 GPU、cpu 实走旧转换，接收持续增长且停止后稳定。
没有注入输入或切换系统 HDR。固定内容场景、主观画质、SDR 真机回归仍未覆盖。
复用测试配置重启出现 trust_store_tampered（BCryptVerifySignature 0xc000a000），
拒绝发生在采集前；保留现场且未修改安全校验，不把基础 A/B 通过描述为可信重连通过。
另用可见合成运动窗口完成两路径重复测量及同进程断开重连；不注入键鼠，动画自动关闭。
静止桌面可能只有首帧/保护期帧，测试帧增长前必须提供明确的画面变化，
不能把无变化时的 DXGI 超时算成转色耗时回归或 GPU 卡死。

随后用户关闭 AMD 主显示器 HDR，完成 SDR auto/cpu 两轮及同进程重连回归：
实际 BGRA8 format=87、actual=sdr，两种参数均绕过 HDR 转换器；1440p 收帧持续，
停止后帧数稳定，最终 Idle。日志中的主动断开 send_failed/session_channel_closed
另保留，不将测试通过表述为全程无错误日志。

用户重新开启 HDR 后完成观察会话：针对切换窗口、滚动、视频及卡住/异常颜色/残影的
检查请求，用户确认没有这些异常，帧数正常。该反馈是人工验收依据，不是客观色彩测量。
观察期间控制端发送画质配置变化及 stopStreaming；采样脚本随后因不再全程 Streaming
而断言失败，保留失败记录，不把人工观察会话称为三分钟自动化全通过。

## HDR 白电平参数回归

`wrc_hdr_display_parameter_tests` 不采集桌面、不修改显示设置；覆盖 80/160/200/320 nit
换算、重复值、零值、查询失败保留和输出重置策略，以及查询 API 非法参数。
真实显示路径匹配及 Windows 亮度滑块变化仍需要设备验证，不能以该单元测试代替。

`wrc_hdr_desktop_mapping_tests`及WARP变体覆盖全65536种FP16位模式的混合RGB、
异常值净化、奇数尺寸、四档白电平与reset，对独立双精度CPU参考最大误差要求<=2。
它同时固定中灰、白及超白阶梯门槛；不替代真实桌面主观观察。
既有旧算法精度测试显式关闭新映射，资源循环默认覆盖新映射，共用纹理生命周期。
