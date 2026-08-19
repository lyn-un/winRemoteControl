# 远程会话安全控制

远程桌面的“控制中心 → 安全”提供两个会话级功能：纯黑隐私屏，以及远程会话最终结束后的工作站锁定。它们不写入 `settings.ini`，每个新 generation 均从 `Disabled / None` 开始。

## 纯黑隐私屏

被控端为每个 `QScreen` 创建一个无边框、置顶、不激活、鼠标穿透的黑色窗口，并对其调用 `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`。任一显示器窗口创建或采集排除失败都会整体回滚，不能以“部分显示器已遮住”作为成功。

遮罩只影响本地可见画面，不应进入 DXGI Desktop Duplication 采集。显示器增加、移除、DPI 或分辨率变化时会重建整组窗口。该机制不控制 UAC Secure Desktop、Windows 锁屏界面或驱动层画面。

隐私屏生效期间注册低级键盘钩子。本地用户可按 `Ctrl+Alt+Shift+P` 紧急恢复；带 `LLKHF_INJECTED` 或低完整性注入标志的事件会被拒绝，因此远程 `SendInput` 不能触发此入口。恢复不依赖网络、React 或控制端响应。

## 会话结束自动锁屏

控制端可为当前会话选择“远程结束后，被控端自动锁屏”。设置动作只负责 arm，不会立即锁屏。被控端只有在同一 generation 曾进入 `Streaming` 后，才会在 Capture 和 Peer 完成异步 teardown 后调用 `LockWorkStation()`，并通过一次性消费防止重复执行。

程序崩溃、被强制结束或操作系统拒绝请求时无法保证锁屏完成。日志中的 `executed` 只表示 Win32 API 接受了请求。

## 权限和能力

- 隐私屏要求 `ViewScreen + InputControl`。
- 自动锁屏要求 `InputControl`。
- C++ 被控端会再次检查权限、状态、generation 和协商能力；UI 隐藏不是安全边界。
- `Reconnecting` 保留隐私屏，不触发锁屏；最终断开才恢复并消费结束动作。
- 旧客户端缺少能力字段时不会显示这些入口，也不会收到安全控制命令。

## DisplayOff 灰度策略

`KWindowsDisplayPowerAdapter` 已实现 `SC_MONITORPOWER` 关闭/恢复，并由独立 `KPrivacyRolloutPolicy` 控制是否发布能力。生产组合根使用默认策略 `bAdvertiseDisplayOff=false`，所以菜单不会出现“关闭显示器”。测试可显式开启策略验证协议与路由。

在真实设备开放前，必须记录以下结果：

1. 笔记本内屏、外接显示器及多显示器至少持续 30 秒的 DXGI 采集。
2. 键鼠输入是否立即唤醒显示器。
3. Modern Standby、电源计划和不同显卡驱动的差异。
4. 会话退出和异常中断后的显示恢复。

验证完成前不能仅通过修改 UI 强行显示入口；应只修改组合根注入的 rollout policy。
