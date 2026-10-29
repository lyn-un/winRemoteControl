# 合成 GPU 源的编码链路基准

`wrc_gpu_encoding_benchmark` 比较发送端 CPU 兼容路径与 GPU 硬件帧路径。
它只在指定厂商适配器上生成可重复的高细节合成动画，不初始化桌面复制，不连接远端，
不注入输入，也不保存桌面或视频内容。它不是双机端到端延迟测试。

## 构建与运行

```powershell
cmake --build --preset release --target wrc_gpu_encoding_benchmark
$env:PATH="$PWD\build\Release;$env:PATH"
.\build\cmake-release\Release\wrc_gpu_encoding_benchmark.exe gpu h264_amf 1920 1080 30 1 --smoke
.\build\cmake-release\Release\wrc_gpu_encoding_benchmark.exe cpu h264_amf 1920 1080 30 1 --smoke
```

位置参数依次为 `cpu|gpu`、`h264_qsv|h264_amf|h264_nvenc`、宽、高、FPS、重复编号。
分辨率只接受 1920×1080 或 2560×1440，FPS 只接受 30 或 60，重复编号为 1..3。
对应厂商适配器不存在时退出 77；有适配器但初始化、编码、PTS 或解码验证失败退出 1，
不能把驱动失败当作“缺少硬件”跳过。参数错误退出 2。正常退出 0 且 JSON 的 valid=true
才是一次有效运行，不能只检查进程有输出。

`--smoke` 固定预热 1 秒、采样 2 秒，结果含 smokeOnly=true，仅验证工具功能。
去掉该参数才是预热 10 秒、采样 60 秒的正式单组运行；程序不自动运行整个矩阵。
完整矩阵为两种路径 × 两种分辨率 × 两种 FPS × 3 次，三个后端分别在可用的真实硬件上
串行执行。不要与编译、另一组编码基准或 GPU 长稳竞争资源后仍当作干净对照。

串行矩阵脚本为 `tools/performance/run-gpu-encoding-benchmark.py`：

```powershell
python tools/performance/run-gpu-encoding-benchmark.py --backend h264_amf --output-dir build/gpu-matrix-amf-run1
```

它只接受 build 下的新目录，不覆盖旧结果；每例保留原始日志和最终 results.json，并
核对退出码、valid、smokeOnly、后端、尺寸/FPS、预热/采样和重复编号。第二次重复采用
GPU→CPU顺序，其他重复CPU→GPU，原始执行顺序保留；单例超时180秒失败，不无限等待。
`--smoke` 只运行功能矩阵，仍不能当正式结果。完整矩阵及中断补测的实际验证状态见文末；
脚本完成消息不代替逐例审核和统计分析。

## 两种路径与校验

| 阶段 | cpu | gpu |
| --- | --- | --- |
| 输入 | 同一固定种子的 GPU 像素着色器，随帧序号变化 | 相同生成规则 |
| 取帧 | staging CopyResource、Map、按 RowPitch 行复制到 BGRA 缓冲 | 保留原生 GPU BGRA 资源 |
| 转换 | 原有 SWS_FAST_BILINEAR BGRA→I420，随后编码器内 I420→NV12 | 实际 KGpuNv12Converter 与有界硬件帧池 |
| 编码 | KH264Encoder 的厂商 CPU NV12 输入 | 同一封装的原生 GPU 输入 |

两种路径均请求 12 Mbps、使用相同 level 上限，保留现有后端码率/GOP/重配置策略。
不使用 BGRA→NV12 单步 CPU 转换代替实际旧链路，也不让 GPU 组偷偷回退为 CPU。
适配器、LUID、驱动原始版本与 FFmpeg 运行时版本进入结果。

绝对时钟控制帧节奏，赶不上时跳过过期采样，不突发补送旧帧；cadenceDropsIncludingWarmup
包含预热和正式阶段，poolDrops 单列硬件帧池背压。提交与输出按实际 PTS 关联，最多
保留64份时间元数据；未知 PTS、帧数不匹配或未排空均失败。

独立线程对整个压缩流连续软件解码，检查尺寸和 PTS 唯一性，最终解码帧数必须与成功
提交数一致。验证队列最多128包/4 MiB，超限判失败，不丢包来凑通过。它不计算真实桌面
的画质。验证解码器现在固定四线程（frame/slice），CPU/GPU 两组相同，配置线程数写入
`validatorThreads`；旧版单线程在 AMF 1440p60 高细节合成源下触发验证队列溢出，
不能将该失败归因到产品编码器。四线程单组复测已完整运行70秒并连续解码4088帧，
队列峰值414719字节。旧单线程矩阵只完成18组，保留为旧工具结果，不拼成新版完整矩阵。

失败输出另含 `validatorError`、`outputs`、`pendingOutputs`、最大压缩包及验证队列峰值，
便于区分编码、PTS 和验证器吞吐问题；退出1和valid=false仍然不能作为有效性能样本。
不计算 PSNR；转色精度由已有离屏颜色测试覆盖，画质验收仍需独立场景。

## 指标边界

- `processingCpuWall`：生产线程从取帧/转换到编码封装返回的墙钟分位数，包含等待；
  不叫“CPU 忙碌时间”。合成源生成在此前提交，但对其 GPU 依赖的等待可能包含其中。
- `submitToEncodedOutputWall`：同帧提交阶段开始到压缩包回调的墙钟时间；不是输入到
  远端显示的延迟。回调可早于函数返回，所以可略小于 processingCpuWall。
- `producerThreadCpuMs`：生产线程真实 CPU 时间；`processCpuIncludingValidatorMs`
  包括软件解码验证及驱动/库的其他线程。两者不能混为编码线程或整个产品 CPU。
- `sampleReadbackBytes`：CPU 组实际成功采样帧的有效 BGRA 字节，GPU 组为0；不统计
  padding 或猜测驱动内部上传/复制量，不能据此宣称所有内部操作完全零拷贝。
- `measuredOutputBpsByMediaTime` 用正式阶段输出字节和该阶段媒体时间计算，不能把请求
  的12Mbps当作测得输出。特别是 AMF 仍有已知严格码率精度限制。
- 资源字段目前记录正式采样结束时的进程私有内存和 GPU 显存，不是峰值，也不等于
  无泄漏结论；资源稳定性另用生命周期/长稳测试判断。
- 后续源码新增 `resourceSampling`：独立线程名义每秒采样私有内存、显存、句柄和线程，
  输出带时间的有界序列及 ObservedMax。它仅表示采样点最大值，可能漏掉短时峰值；
  该诊断线程成本计入进程 CPU，生产线程 CPU 不包含它。缺失值为 null。
  仍未测 GPU 引擎利用率，`gpuUtilizationMeasured=false`，不能用显存占用替代利用率。
  此改动尚待编译验证，运行中的旧矩阵没有该字段。
- 新增 `gpuFramePoolCapacity` 与 `gpuFramePoolPeakInUse`，后者在申请槽位边界观察
  AVBuffer 引用保留情况；异步释放可与扫描交错，因此不是硬件引擎在任意时刻的精确占用。
  固定容量仍为8，不增加缓冲或内存分配；专项探针覆盖占满和换代后的统计重置，尚待重编实跑。
- 源码已补充完整采样时长检查及 `sampleWallSeconds`，提前终止的用例不得通过；
  解码验证的失败状态只锁存为失败，不能被后续成功包覆盖。这两项加固尚待重编验证，
  已启动的矩阵仍使用加固前二进制，不混写为同一构建的测试结果。
- 矩阵脚本新增 `manifest.json`：记录可执行文件/脚本 SHA256 和各用例进程墙钟时长，
  每次启动用例前核对二进制未变；变更时拒绝继续拼接不同构建的结果。
  脚本语法与帮助检查通过；已经运行中的旧脚本没有该清单，不能追溯伪造逐用例时长。

矩阵脚本的无 GPU 合同测试可单独运行：

```powershell
python tests/unit/gpuencodingmatrixrunnertest.py
```

该测试用伪子进程覆盖24组顺序/清单、无效结果拒绝、二进制变更拒绝和硬件缺失显式跳过；
2026-10-27四项通过。它不执行编码器，不替代性能矩阵、基准 C++ 或硬件验证。

## GPU timestamp 微测量

### 外部 GPU Engine 计数器补充

Windows的 `GPU Engine` 性能计数器已验证可读取本机基准PID：一次探测返回14个有效引擎
计数，包含3D与Video Codec。后续AMF运行使用独立PowerShell采样器按矩阵驱动的父子PID
关系绑定，只读取该驱动创建的基准进程，保存时间、模式/尺寸/FPS/重复及各引擎原始数值/状态。
原始数据在忽略的build目录，不写入常驻应用日志；不记录桌面或输入。

这是部分覆盖的外部补充，不改变基准JSON中的 `gpuUtilizationMeasured=false`；不能把
未采样的旧用例补成已测量，也不把不同引擎简单相加称作整卡GPU百分比。外部采样进程的
CPU不计入基准进程CPU，计数器观测仍可能影响系统调度，分析时保留该条件。

编码阶段的 gpuExecutionMeasured=false：没有把 CPU 提交耗时伪装成编码器 GPU 时间。
随后独立运行 `gpuConversionMicrobenchmark`，在相同适配器上测量 RGB→NV12 的
输入复制和 Y/UV 绘制命令跨度，不包含硬件视频编码引擎，也不包含合成源生成。
该阶段在编码采样、CPU 计时和资源快照之后执行，不能加进端到端耗时。

微测量运行512次转换、前16次不采样，使用8组 disjoint/start/end 查询，后续循环以
DONOTFLUSH 延迟读取。未完成的槽位不覆盖、不等待，继续执行无测量转换；末尾不强等
GPU，结果显式记录 busyQuerySlots、invalidQueries 和 tailExcludedWithoutWait。
available=false 不是零耗时。gpuCommandSpan 是 GPU 时钟上的命令跨度，不是整条链路的
纯 shader 忙碌时间。少量样本的 p99 不能当作稳定尾延迟结论。

## 当前验证状态

最新更新：AMF新版四线程验证器完整24组已全部有效、连续解码通过；NVENC同构建24组合
也已补齐，但包含一次驱动/观测中断（原16组+补测8组），不得表述为连续不间断矩阵。
两者均记录GPU读回0、池满丢帧0；CPU与处理p95下降并不等于FPS全面改善，NVENC1080p60
出现更多节拍跳过。AMF实际输出仍显著偏离请求12Mbps，此既有精度限制未在本轮修复。
AMF外部GPU计数只有部分覆盖及观测缺口，主基准不伪造缺失值。

2026-10-27更新：普通Release全量构建已通过；完整CTest101项，75通过、26显式跳过、
0失败（115.21秒），另显式执行本机AMF/NVENC硬件帧/WebRTC及设备缓存专项7项全部通过，
跨卡共享诊断探针另显式通过（当前两张卡的四项跨卡打开均失败，同卡四项数据正确）。
新版GPU冒烟的帧池容量8、申请边界观测占用峰值1，并得到两份资源采样。
远端Intel旧单线程验证器的QSV完整24组均连续解码通过，汇总/清单已取回并校验SHA256，
每组进程墙钟至少70.47秒；60FPS场景仍有节拍跳过，不宣称所有用例稳定满帧。
AMF旧矩阵18组后因验证器溢出终止，不与新版24组混合；NVENC原16组加补测8组，
中断情况显式保留。历史冒烟及其限制保留在下文，不代表当前全部状态。

本机 AMF 的 CPU/GPU 1080p30 冒烟各成功提交并连续解码90帧，退出0；延迟 timestamp
微测量获得有效结果，无查询错误。它们是工具功能检查，且当时另一张 NVIDIA 卡在跑
真实长稳，因此不用于收益百分比或正式矩阵对比。三后端正式矩阵状态见上文；资源连续峰值、
完整GPU利用率覆盖及异常注入仍未齐备，不把已编译/有限采样写成全部性能验收完成。
