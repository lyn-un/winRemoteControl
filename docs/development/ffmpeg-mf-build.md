# 项目专用 FFmpeg 与 MF 动态码率

> **历史实验，2026-10-10 更新：** Intel 的 `S_FALSE` 曾被 MF 补丁误判为成功，AMD 实际输出也未通过验收。活动 MF 补丁、helper 测试及旧构建脚本现已移除；`tools/ffmpeg/build.ps1` 已转向[厂商后端构建](ffmpeg-vendor-build.md)，其中 `patched` 现在仅表示 AMF 补丁。下文命令/路径只用于解释历史记录，不再是可执行的 MF 构建说明。普通 Release 已换为 `wrc-vendor-patched-91848b6d469ad7c0`，九个运行 DLL 与依赖匹配；新源码的 `mfenc.c` 与干净基线一致。撤销不代表 MF 动态码率已修复。

项目仍使用 FFmpeg 公共 API；MF 修复位于第三方 `libavcodec/mfenc.c`，不从应用中访问私有布局。
原 `third_party/ffmpeg`（Gyan 8.1.1）保持不变。补丁源码基于该包标注的 `239f2c733d`，
x264 固定为 `c24e06c2e184345ceb33eb20a15d1024d9fd3497`，NASM 固定为 2.16.03。
脚本内保存下载 SHA256；FFmpeg/x264 为 GPL 组件，发布时必须继续满足许可证及对应源码提供要求。

## 构建

使用现有 MSYS2 UCRT64 的 GCC、make、pkg-config、patch、curl、tar；缺少工具时报错，不自动安装。
NASM 官方便携包仅解压到 `build/mf-bitrate/tools`，只影响构建子进程 PATH。

```powershell
./tools/ffmpeg/build.ps1 -Mode original
./tools/ffmpeg/build.ps1 -Mode patched
# 网络需要代理时，给脚本加 -Proxy http://127.0.0.1:端口
cmake --preset release -DFFMPEG_ROOT="$PWD/third_party/ffmpeg-wrc-patched"
cmake --build --preset release
```

两个构建分别安装到 `third_party/ffmpeg-wrc-original`、`third_party/ffmpeg-wrc-patched`；
保持相同配置，补丁版仅多 MF 补丁及可识别的版本后缀。保留 H.264 MF/x264 编码、H.264 解码/解析、
swscale、swresample 和 D3D11 支持。不是通用 ffmpeg 命令行发行包。
头文件、导入库、DLL 必须来自同一安装目录；不要手动混合原包的 DLL。
配置清单与许可证随库保存。若重新编辑补丁，请使用新的构建目录或人工确认清理对应生成源码后重新应用，
不要在已经打补丁的源码上重复应用。只部署普通 `build/Release`。

恢复原依赖：重新执行 `cmake --preset release -DFFMPEG_ROOT="$PWD/third_party/ffmpeg"` 并全量重建。
切换依赖后需要退出正在运行的程序，否则 Windows 可能锁定旧 DLL。

UCRT64 构建的 avutil 还依赖 `libwinpthread-1.dll`，脚本连同许可证复制到依赖目录，
CMake 随主程序和视频基准部署；不要只复制四个 FFmpeg DLL。构建仅在子进程固定 ASCII HOSTNAME，
规避中文主机名的临时文件名问题，同时固定 FFmpeg REVISION，避免误取外层应用仓库版本。

## 更新语义

初始化检查平均码率设置结果，运行时在下一张有效视频帧前调用 `ICodecAPI::SetValue`，
设置 `CODECAPI_AVEncCommonMeanBitRate`（VT_UI4，bit/s）。当前实现仅用 `FAILED(hr)` 判错，
因此会错误地把表示只读的 `S_FALSE` 也保存为已接受目标；这是已知缺陷，不能据日志中的 accepted 认定设置成功。
相同目标及非正值不调用，超出 UINT32 范围拒绝；排空时不更新。失败锁定本编码实例的错误，
沿用编码失败报告，不每帧重试，不新增运行时软件回退、不重建编码器、不主动请求关键帧。
低延迟 VBR、GOP、画面采集和接收显示均保持不变。

`mf_rate_update` 表示 API 接受或拒绝参数，不是测得的输出码率。驱动接受更新也可能不遵守目标，
必须通过实际编码验收。初始化失败仍沿用原有 Auto 初始化回退策略。

## 验证

补丁构建会从实际源码提取更新 helper，使用模拟 ICodecAPI 检查重复目标、非正值、UINT32 边界、
200 次更新、无接口及失败锁定；不会用重新实现的逻辑替代被测函数。

```powershell
$env:WRC_RUN_MF_BITRATE_TEST='1'
ctest --preset release -R '^wrc_mf_bitrate_output_tests$' -V
Remove-Item Env:WRC_RUN_MF_BITRATE_TEST
ctest --preset release --output-on-failure
```

硬件测试默认跳过；显式开启但没有枚举到硬件 H.264 MFT 时返回跳过，其余初始化或编码错误为失败。
使用 1280×720/30fps 固定种子动画，动态 1→6→1 Mbps 及固定低/高对照，每阶段 300 帧，
舍弃前 60 帧，按媒体时间计算码率。按输出 PTS 关联原始图像，不假定同步出帧；最终排空后检查 900 帧。
高阶段至少为前低阶段两倍、回落不超过高阶段 60%、对应固定对照误差不超过 30%、高阶段亮度 PSNR
不低于低阶段。阈值不因驱动表现下降而放宽。

原发行包、同配置未打补丁、补丁版分别运行并记录。GPU/驱动/MFT 和结果写入本机忽略的
`performance-baseline.md`；原始日志保留在 `build/mf-bitrate`。硬件测试通过不代表全部显卡通过，
也不代表双机窗口切换模糊已验收；用户出现问题的那台被控机需另行复验。

## 当前硬件验证限制（2026-09-29）

本机 AMD Radeon 驱动 32.0.12033.1030、`AMDh264Encoder` 的原发行包与未打补丁自建库结果一致：
动态 1→6→1 Mbps 的输出仍与固定 1 Mbps 对照相同。补丁版 SetValue 成功，输出变为约
3.247→17.992→3.001 Mbps，证明更新链路有响应，但中间阶段与固定 6 Mbps 对照（5.974 Mbps）
偏差超过 30%，因此硬件验收失败。不得据此宣布 MF 码率控制已完全修复，亦不使用除以三的驱动补偿。
默认 AMD 自动选择软件编码的既有策略保持不变；强制 MF 仅用于后续诊断。其他机器和双机画质尚未验收。

普通 Release 全量构建通过，默认完整 CTest 58 通过、2 跳过（真实输入、MF 硬件专项）。
x264 实际码率回归仍通过；上述 MF 硬件专项是在显式启用后单独执行并失败，不能被默认跳过掩盖。
