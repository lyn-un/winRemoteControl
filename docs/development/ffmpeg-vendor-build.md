# FFmpeg 厂商后端构建与验收状态

## NVENC 应用接入（2026-10-16）

复用现有 `abb8e70a1c7802e6` DLL，无新增FFmpeg补丁。应用增加显式NVENC及auto候选、
D3D11适配器身份绑定、低延迟CBR和运行时码率字段更新。当前上游动态重配置设置
resetEncoder/forceIDR；失败只输出库错误，应用转发到session trace的ffmpeg_error。

本机NVIDIA GeForce RTX 4060 Laptop GPU（10de:28e0，驱动raw=9007199255732212）
严格720p30高细节测试：动态1→6→1 Mbps，稳定段约1.03159→6.000→1.03159 Mbps，
PSNR约14.39→24.50→14.39 dB；动态及固定1/6 Mbps三组各900帧完整解码，输出profile=578。
200次调整不重建实例通过。六种尺寸/FPS的低码率启动测试通过，包括1440p30。

频繁更新专项每秒切换3/4 Mbps，30秒媒体时间完整解码900帧、IDR30个；固定3 Mbps
对照IDR15个。前者最大包130978字节、100ms媒体码率峰值约12.462 Mbps，后者99239字节、
约11.046 Mbps。频繁组PSNR约15.62 dB，固定组约19.14–19.19 dB；表明本场景下频繁
重配置存在明显画质代价。该结果不是实际网卡峰值或端到端延迟测试。

20次生命周期观察句柄283→325、私有内存91131904→97894400字节，保留为资源增长待查项；
不能宣称无泄漏。该测试的1080p60节拍阶段完整输出600帧。历史AMF严格低码率失败仍保留。

普通Release全量构建通过；完整CTest为67通过、11显式跳过、0失败（78项，121.81秒）。
本机AMF六组低码率启动回归通过；远端Intel QSV严格码率回归退出0，动态输出约
1.232→6.175→1.008 Mbps，三组各900帧连续解码通过。未更改原验收阈值。

Intel控制本机：默认NVENC、adapter0，无回退；实际HDR FP16通过既有GPU路径转SDR。
720p切换1440p后持续采样600秒，接收统计5595帧全部解码、丢包0、丢帧0，采集/编码
错误0。原生窗口消费计数停止在5592（与WebRTC接收统计属于不同测量点）。长测后的
首次重连脚本因未消费事件导致历史游标过期，修正测试脚本后单独补测：双档位停止计数
746，重连停止计数124，帧计数及时间戳均稳定。反向QSV短测停止计数733/174，通过。

两方向断开后均复现既有send_failed/session_channel_closed状态错误。动态合成窗口
及静止桌面验证不等于恒定30FPS吞吐或主观画质验收；未注入键鼠、未验证实际网页滚动
及视频播放器场景。NVENC真实1440p实例5280次提交/输出中有593个关键帧，频繁码率
更新的影响仍需后续优化。未修复本轮发现的资源增长。两端测试进程已正常退出。

## AMF Main Profile 隔离对照（2026-10-11）

经用户单独确认，在忽略的 `build/amf-main-profile` 编译应用编码器与硬件测试副本。
编码副本唯一变化是AMF profile选项由constrained_baseline改为main；测试副本仅将实际
解码Profile预期改为Main，输入、1→6→1 Mbps阶段、固定对照与数值门槛不变。
独立CMake入口复用当前FFmpeg与静态依赖，未修改正式应用/SDP、未重编FFmpeg或部署实验程序。

同一AMD 1002:1681、驱动raw=9007200043336710、FFmpeg构建abb8e70a1c7802e6：
Main实际profile=77，三组各900帧连续解码；200次更新不重建，三组IDR仅首帧。
Main动态输出约2.301→5.984→2.301 Mbps，高/低约2.60倍，回落约38.5%，固定对照偏差小于1%，
高段PSNR高于低段，原数值响应门槛通过（退出0）。Baseline复跑仍约3.440→6.008→3.439 Mbps，
退出1。两者低段均QP51；Main低段仍比请求1 Mbps高约130%，固定1 Mbps同样如此。

结论只限本硬件/驱动和高细节合成内容：Profile显著影响压缩效率，动态更新已有实际响应；
不能把Main相对门槛通过解释为目标码率精确跟踪，也不能替代正式Constrained Baseline验收。
保持正式Profile、协议、驱动与全部原阈值；当前AMF正式验收仍未通过。
实验源和日志在 `build/amf-main-profile`，详细数据追加本机忽略的根目录性能基线。

## AMF 参数对照实验（2026-10-11）

在同一本机 AMD 1002:1681、相同严格合成输入和阈值下，额外测试了 CBR 显式
`enforce_hrd=1/frame_skipping=0`，以及低延迟 VBR (`vbr_latency`)。
前者动态 1→6→1 Mbps 实测约 3.440→6.008→3.439 Mbps；后者约
3.451→5.614→3.450 Mbps，两者均未达到高段至少为前低段两倍的要求。
CBR 低段 QP 全为51；低延迟 VBR 低段约99.8%的块为51。
这支持“该高细节输入下存在低码率控制极限”的判断，但不证明所有 AMD 设备都有此限制。
实验参数已撤回，仍保留 CBR；未将实验参数部署到正常程序，未降低验收阈值。
原始日志为忽略目录中的 `build/ffmpeg-vendor/amf-hrd-probe.log` 和
`amf-lcvbr-probe.log`。AMF 严格验收仍未通过。

## QSV 修正后的 Intel 验证（2026-10-11）

补丁构建指纹 `abb8e70a1c7802e6`，仍为固定 `239f2c733d` 基线；AMF 原补丁保留、MF 无补丁。
可选接口 helper 5项通过，AMF helper 通过，五编码器枚举通过。Intel Iris Xe
8086:a7a0 / 驱动31.0.101.4146 / VPL运行时2.8，未更新驱动或更换GPU。

实测定位了三个独立问题：原依赖卡在可选接口查询；仅加补丁后可以初始化，但 SPS 为
`42001f`（普通Baseline）；旧动态缓冲配置则在300→235 kbps或31→32 KB时导致
Reset返回 `MFX_ERR_INCOMPATIBLE_VIDEO_PARAM (-14)`。应用改为明确Constrained Baseline、
关闭QSV CBR的HRD、初始化预留固定缓冲预算后，以下专项通过：

- 等级/实际SPS/连续解码：1280×720p30、1920×1080p30、1920×1080p60、1152×720p30、
  1920×1200p30，各12帧。
- 低码率启动与预算增长：同五组，各120帧，300→235→6000→235 kbps，完整解码。
- 同一实例200次调整不重建，非正值/重复值/整数边界回归通过。
- 严格1280×720p30高细节合成输入，1→6→1 Mbps输出约1.232→6.175→1.008 Mbps，
  高段/前低段约5.01倍，回落约16.3%；与对应固定对照偏差分别0%、2.10%、0.97%。
  亮度PSNR为14.08→23.98→11.49 dB；三组对照各900帧连续解码通过。
  动态与固定对照IDR都在PTS 0,60,…,840，未增加额外IDR位置；不代表所有驱动行为。
- 1080p60节拍合成600帧全部输出；20次生命周期后句柄217→217，私有内存约59.8→98.2 MB，
  仅作短程观察，不能宣称无内存增长或无泄漏。

远端原始记录位于 `D:\projects\winRemoteControl\build\intel-diagnostics\qsv-*.log`。
这些测试不采集桌面、不注入输入，不证明双机真实画质、FPS或端到端延迟。
AMF严格码率失败仍是独立未解决项；NVENC仍仅编入。

最终普通Release全量构建通过，仅更新本机 `build/Release`。完整CTest 66项：58通过、
8按开关跳过、0失败，109.25秒；真实输入关闭。AMD显式等级与启动专项也通过，
不改变此前AMF高细节严格码率失败结论。`git diff --check`通过。

远端 `D:\Qt_Projects\winRemoteCtrl\Release` 已更新主程序及完整9个依赖DLL，
全部逐文件核对SHA-256；程序哈希 `17FDD33AA7111594D36E016A9F2645F381BBDEE42BD656B0D49BA946575AE754`。
仅从此运行目录加载依赖后，最终等级、启动、严格码率专项再次全部通过。
替换前备份位于远端 `build/intel-diagnostics/runtime-backup-20261011-135024`；
未覆盖远端旧源码、配置、配对数据和日志，未自动启动主程序或建立远控会话。
未提交或推送。下一步由用户同意并启动双机真实桌面会话复验。

## Intel 远端首次复现（2026-10-11）

通过用户授权的远端 MCP 在 Intel Iris Xe（8086:a7a0，驱动 31.0.101.4146）
运行合成画面专项。测试程序位于远端源码目录下的 `build/intel-diagnostics`，
PATH 使用用户指定的 `D:\Qt_Projects\winRemoteCtrl\Release` 与 Windows 系统目录。
主程序、测试程序及 avcodec/avutil/libvpl 的 SHA-256 与本机对应文件一致；
远端源码仍为旧提交 ab63d2b，未覆盖或作为本次构建来源。

等级专项和严格码率专项均返回 1（失败，不是缺硬件跳过），在编码器打开阶段报：

```text
Error getting mfx config interface handle: specified object was not found (-9)
Open h264_qsv failed: Function not implemented
```

固定 FFmpeg 基线的 `qsvenc.c` 在 `HAVE_STRUCT_MFXCONFIGINTERFACE` 条件下
无条件查询 `MFX_HANDLE_CONFIG_INTERFACE`，之后才遍历 `qsv_params`；当前应用未使用
该选项。这是本次执行环境下已复现的初始化兼容缺口。此前双机日志曾初始化成功，随后
分别出现提交帧 EINVAL 和 SPS profile/structure 拒绝，因此不能把这三个现象直接视为
同一根因；修复初始化后仍需单独复现和验证后两项。

用户于 2026-10-11 单独确认新增最小 QSV 兼容补丁：
`tools/ffmpeg/qsv-optional-config-interface.patch` 仅在确实存在 `qsv_params` 时
查询可选接口；实际请求该接口时仍保留原来的错误处理。不改变 QSV 动态码率逻辑，
不升级驱动、不关闭 SPS 检查。配套 helper 直接编译补丁中的条件块，覆盖未指定参数、
接口缺失、其他错误和正常返回。构建和远端硬件结果见后续验证记录，不能以 helper
通过代替驱动验收。原 AMF 严格码率失败及双机验收状态不变。

## 当前状态

2026-10-10：已加入隔离构建、五编码器枚举、QSV/AMF 显式应用接入、设备绑定、异步输出 PTS 对应和 AMF 动态补丁。模拟接口测试已通过。首轮 AMD 实测确认补丁使码率发生响应，但严格验收失败：1 Mbps 的高细节合成输入实际输出约 3.44 Mbps，高阶段约 6.01 Mbps，未满足两倍比例；未放宽阈值。Intel 硬件及双机画质尚未验证，NVENC 仅编入。完整 Release 部署和最终回归状态以本文后续验证记录为准，不把代码接入当作硬件验收通过。

## 构建入口

在项目根目录运行：

```powershell
./tools/ffmpeg/build-vendor.ps1 -Mode patched
# 网络需要代理时，显式传入 -Proxy http://127.0.0.1:端口
```

这里 `patched` 包含 AMF 动态码率补丁和 QSV 可选配置接口兼容补丁，`build.ps1` 也已转发到此入口，不再构建旧 MF 实验。`-Mode original` 用于相同依赖/配置的干净上游对照。两者都不应用 MF 补丁；运行目录替换仍须另行验证。

- 需要现有 MSYS2 UCRT64 GCC/G++、make、pkg-config、patch、dlltool，以及 CMake、MSVC 2022 x64、Windows SDK。
- 缺失工具会报错，不自动安装系统软件或升级驱动。NASM 使用锁定的便携归档。
- 源码归档 URL、版本及 SHA-256 位于 `tools/ffmpeg/vendor-dependencies.json`。哈希为下载归档的固定校验值，不宣称是上游发布的签名。
- VPL dispatcher 用 MSVC 构建共享库；FFmpeg/x264 用 MSYS2 UCRT64 构建。FFmpeg 的公共头文件、导入库与 DLL 必须来自同一输出。
- 源码、配置脚本、补丁和 GCC 版本参与目录指纹，修改后使用新目录，不复用旧 patched 源码。FFmpeg 补丁应用禁止 fuzz，并比较 `mfenc.c` 与干净归档一致。
- 中间产物在 `build/ffmpeg-vendor/<指纹>`，安装在 `third_party/ffmpeg-wrc-vendor/<指纹>/<模式>`，脚本末尾打印 `FFMPEG_ROOT`。通过 `cmake --preset release -DFFMPEG_ROOT=<完整目录>` 接入并重建。
- 两个目录均已被仓库忽略。不得把许可证、清单或必要运行依赖留在开发机而遗漏部署。

## 版本选择依据与运行要求

| 依赖 | 锁定版本 | 选择依据/限制 |
| --- | --- | --- |
| FFmpeg | `239f2c733d` | 保持现有源码基线，隔离后端接入与版本升级影响 |
| x264 | `c24e06c2e184345ceb33eb20a15d1024d9fd3497` | 保持现有 VBV 修复对照 |
| VPL dispatcher | 2.13.0 | 满足本基线 configure 的 VPL >= 2.6；编译通过不证明目标 Intel 代际/驱动兼容 |
| AMF 头文件 | 1.4.36 | 满足本基线最低头文件版本；运行时来自 AMD 驱动，不部署头文件包中的驱动或样例 |
| nv-codec-headers | 12.1.14.0 | 属于本基线接受的版本范围；该包 README 要求 Windows NVIDIA 驱动 >= 531.61，仍须实际设备能力检查 |
| NASM | 2.16.03 | 沿用现有便携汇编器 |

AMF/Intel 最低可用驱动不能仅根据头文件版本给出统一数字，应记录具体 GPU、驱动、加载到的运行时与初始化结果。未完成 Intel 目标机验证之前不声称覆盖其硬件代际。NVENC 本轮只编入与枚举，不开放应用选择。

## AMF 补丁语义

- 只处理 H.264，其他 AMF 编码格式不启用新增动态行为。
- 初始化检查码率模式及目标/峰值/VBV 设置结果；在提交新有效帧前比较缓存与请求参数，变化时才调用 AMF `SetProperty`。
- 升码率先提高峰值，降码率先降低目标。参数使用 bit/s 和 bits，不做隐式倍率补偿；不切换码率模式、不重建编码器、不主动请求 IDR。
- 暂停时非正目标不清空有效配置；非法峰值、缓冲或运行时把显式峰值/缓冲重置为默认会报错。
- 多属性更新不是事务：任何一步失败后缓存不标记整组成功，锁存编码错误，后续不持续重试。驱动可能已经接受部分属性，因此不继续提交新帧。
- 日志 `amf_rate_update apiAcceptedTargetBps=...` 只代表 API 接受，不代表实测输出；错误日志保留属性、请求值及 AMF_RESULT。实际编码测试必须单独报告输出字节、媒体时间、PSNR 与连续解码。

## 分层验证

### 当前兼容性缺口

用户随后已授权单独修正 level 协商，已实现集中策略、配置传递与 SPS 检查。
不得通过降低分辨率/FPS、篡改 SPS 声明或放宽验收门槛消除表面不一致。
下列 level 4.2/3.1 结果属于修正前的历史记录；新版本验证在文末另记，双机验收仍单独要求。

首轮 AMD 720p30 测试连续解码成功，输出为 Constrained Baseline（FFmpeg profile=578），
但驱动自动给出的 SPS level 为 4.2，而当前 SDP 仍声明 3.1。本轮没有修改协商协议，
因此不能把本机 FFmpeg 解码成功表述为所有接收端兼容性通过。后续需处理 level 约束与
实际分辨率/帧率的对应；不能直接把所有 1080p60 流强行标为 level 3.1。
动态和固定对照的关键帧标记都只在首帧出现；动态更新未新增关键帧，仍需双机丢包恢复验证。

首轮未完成项包含 AMF 低码率偏差、SPS level 和 Intel 硬件验证。当前 level 本机专项已通过，
其余两项及双机协商仍未完成；保留显式诊断入口，不修改默认选择。

后续 QP 诊断确认：低/回低稳定段各 864000 个宏块全部 QP=51；高阶段 QP 范围 34～48、
平均 39.5662，QP=51 占比为零。固定低码率对照同样达到 QP=51。
因此“仍未转发码率”不能解释这个低段偏差：当前 speed/ultralowlatency 配置下，
这组高细节输入已达到量化上限。这个结论不代表其他配置或其他内容也不可达，
也不把量化饱和当作原验收通过；保留原输入和阈值，不引入跳帧或倍率补偿。

仅测试构建把 AMF `quality` 从 `speed` 临时改为 `quality` 的单变量对照也失败：
同一输入的动态输出为 3.324→6.023→3.324 Mbps，低/回低仍全部宏块 QP=51，
高/低比例仍不足两倍。三组各 900 帧均连续解码，SPS level 仍为 4.2。
因此该预设替换不足以解决此硬件/输入下的低码率偏差；实验后恢复 `speed`，
未部署试验预设到普通运行程序。原始日志 `build/ffmpeg-vendor/amf-quality-probe.log`。

### 测试命令

```powershell
# 源目录须为已应用补丁的 FFmpeg 源码。
C:/msys64/usr/bin/bash.exe tools/ffmpeg/test-amf-rate-update.sh <源目录>
C:/msys64/usr/bin/bash.exe tools/ffmpeg/test-qsv-config-interface.sh <源目录>
```

模拟测试编译从补丁源码提取的实际函数，覆盖初始化、200 次调整、重复目标、升降顺序、CBR/VBR 映射、整数边界、非法参数、逐项失败及错误锁存、非 H.264 不受影响。它不加载 GPU，也不证明帧提交入口和硬件行为；这两部分必须通过库构建与实际编码验证。

`tools/ffmpeg/vendor-probe.c` 只检查五个编码器是否编入，不能据此判断机器能否硬件编码。完整验收仍遵循[接入规划](../plans/vendor-h264-encoders.md)，包括未打补丁/打补丁对照、同一实例 1→6→1 Mbps、固定码率对照、PTS 对齐、连续解码和双机画质复验。

应用切换时只更新普通 `build/Release`；必须部署匹配的完整 DLL 集、VPL dispatcher 与必要工具链运行库，核对哈希和 DLL 依赖闭包。恢复时重新指定旧的完整 `FFMPEG_ROOT` 并重建，不混换单个 DLL，不覆盖用户安全数据。

运行库包含 FFmpeg 四个 DLL、`libvpl.dll`、`libwinpthread-1.dll` 和 VPL 安装附带的
`msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll`。AMD/Intel/NVIDIA 驱动由目标机器安装，
不复制开发机的驱动 DLL。构建结束在移除开发工具 PATH 的环境运行只读枚举探针，输出
`wrc-encoder-enumeration.txt`；此项仍不替代无开发环境机器上的应用实测。

## 2026-10-10 本机验证记录

- 最终依赖标识：`8.1.1-239f2c733d-wrc-vendor-patched-91848b6d469ad7c0`。
- 五编码器编入并可枚举；MSVC 导入库与头文件/DLL 来自同一构建。普通 Release 全量构建通过，
  仅更新 `build/Release`；九个运行 DLL 哈希匹配，许可证和依赖清单随运行目录部署。
- MF 源码与干净基线 SHA-256 一致，旧动态补丁未应用；原发行包与历史对照目录未删除。
- 默认完整 CTest：58 通过、4 跳过、0 失败，88.69 秒。真实输入、MF、QSV、AMF 专项默认关闭。
  首次沙箱内运行受到临时文件/CNG 权限限制，沙箱外重跑消除这些环境失败。
  补充 QP 诊断及缺设备拒绝测试后再次完整回归：58 通过、4 跳过、0 失败，88.84 秒；
  日志 `build/ffmpeg-vendor/qp-final-ctest.log`。该默认回归不包括显式 AMF 失败专项。
- AMF 显式专项仍失败：实际输出约 3.44→6.01→3.44 Mbps，固定对照一致但未达两倍门槛，
  低阶段明显高于 1 Mbps。200 次调整、三组各 900 帧连续解码和 PTS 检查通过；IDR NAL 只在首帧，
  未观察到码率变化新增 IDR。不能把 API 接受、输出有响应或默认 CTest 通过表述为 AMF 全验收通过。
- QSV 显式检查因无 Intel GPU 跳过；跳过前实际尝试打开 QSV，确认因找不到 `0x8086`
  适配器而拒绝初始化，没有回退到软件编码。此项只验证失败路径，不算 QSV 硬件码率通过。
  原始记录：`build/ffmpeg-vendor/qsv-missing-device-check.log`。NVENC 没有应用接入及硬件验收。
  双机画质/延迟未测。
- AMF 1080p60 合成节拍与 20 次启动/释放完成，输入/输出 PTS 完整。句柄未增长，
  私有内存仍有增长；这是短程资源观测，不宣称无泄漏或优于 x264。
- 清空开发工具 PATH 后只读枚举探针通过，主程序用已知无效编码器参数退出 2（预期），
  未创建远程会话。`--help` 在无控制台环境可能弹 Qt 帮助框，不作为自动退出测试使用。
- 详细性能/资源数值仅追加到本机忽略的根目录 `performance-baseline.md`，原始日志在
  `build/ffmpeg-vendor/`；不提交个人基线或日志。

上述阶段仍是显式诊断后端；2026-10-15 起 `auto` 改为按显卡实际尝试 QSV/AMF 后回退 x264，
详见视频链路文档。AMF 低码率精度和长期双机稳定性仍需单独跟进；
本次默认选择调整不将这些历史未通过项改记为通过。

## level 修正的独立验证（2026-10-10）

集中规则和协商映射已接入，未重编 FFmpeg。软件路径实际编码/解码覆盖 720p60、1080p60、4K60，
旧端 3.1 协商对照与不兼容 SPS 拒绝通过。level 5.2 是当前声明的支持上限，不是所有输出都标为 5.2。
AMD Radeon(TM) Graphics（0x1002:0x1681）独立专项通过：720p30、1080p30、1080p60
在 3.1/4.0/4.2 上限内输出并各连续解码 12 帧。最终记录 `level-final-hardware.log`。
QSV 因无 Intel GPU 跳过；以上没有双机网络、桌面采集、实时性能或输入延迟验收。
原 AMF 严格码率失败不因等级专项通过而变为成功；MF 不消费 level 配置的限制继续保留。

普通 Release 全量构建通过，仅更新 `build/Release`。最终完整 CTest 共 64 项：
58 通过、6 跳过、0 失败，耗时 89.25 秒；默认关闭真实输入及硬件测试，
AMD 专项的显式运行结果另见上文。记录为 `level-final-ctest.log`，`git diff --check` 通过。

追加部署 DLL 检查：测试程序目录无 DLL，进程 PATH 仅保留 `build/Release` 与 Windows
系统目录后，AMF 三组等级往返再次通过，QSV 明确缺少 Intel GPU 并返回 77，未软件回退。
记录为 `level-deployed-runtime-amf.log` / `level-deployed-runtime-qsv.log`。
这排除了本次测试依赖开发目录 DLL 的问题，但不是无开发环境新机器验收。
另一台机器的独立命令见[构建与测试：另一台机器无开发环境时的复验](build-and-test.md#另一台机器无开发环境时的复验)。
