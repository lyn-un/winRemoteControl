#include "session/sessionerrorpresenter.h"

QString KSessionErrorPresenter::userMessage(const KSessionError &error)
{
	if (error.code == RemoteAccessDisabledSessionErrorCode)
		return QStringLiteral("对方已关闭远程控制");
	if (error.code == RemoteBusySessionErrorCode)
		return QStringLiteral("对方正在处理其他连接");
	if (error.code == ApprovalRejectedSessionErrorCode)
		return QStringLiteral("对方拒绝了远程控制请求");
	if (error.code == ApprovalTimeoutSessionErrorCode)
		return QStringLiteral("等待对方确认超时");
	if (error.code == IncompatibleProtocolSessionErrorCode)
		return QStringLiteral("两端程序版本不兼容，请升级后重试");
	if (error.code == ConnectionTimeoutSessionErrorCode)
		return QStringLiteral("连接超时，请检查网络后重试");
	if (error.code == ConnectionFailedSessionErrorCode)
		return QStringLiteral("无法连接到被控端，请检查地址和网络");
	if (error.code == ConnectionLostSessionErrorCode)
		return QStringLiteral("连接已中断，可以重新连接");
	if (error.code == RecoveryFailedSessionErrorCode)
		return QStringLiteral("网络恢复失败，可以重新连接");
	if (error.code == InputBackpressureOverflowSessionErrorCode)
		return QStringLiteral("网络拥塞过重，已安全停止输入");
	if (error.code == CommandTimeoutSessionErrorCode)
		return QStringLiteral("远端未确认控制命令，会话已安全停止");
	if (error.code == CommandQueueOverflowSessionErrorCode)
		return QStringLiteral("控制命令队列已满，会话已安全停止");
	if (error.code == ShutdownTimeoutSessionErrorCode)
		return QStringLiteral("会话关闭超时，请重新启动程序");
	if (error.code == CaptureFailedSessionErrorCode)
		return QStringLiteral("屏幕采集失败");
	if (error.code == TerminalUnavailableSessionErrorCode)
		return QStringLiteral("远程终端当前不可用");
	if (error.code == TerminalApprovalRejectedSessionErrorCode)
		return QStringLiteral("被控端拒绝了终端请求");
	if (error.code == TerminalInputOverflowSessionErrorCode)
		return QStringLiteral("终端输入拥塞，已安全关闭终端");
	if (error.code == TerminalOutputOverflowSessionErrorCode)
		return QStringLiteral("终端输出拥塞，已安全关闭终端");
	if (error.code == TerminalHostStartFailedSessionErrorCode)
		return QStringLiteral("无法启动远程 PowerShell");
	if (error.code == TerminalRelayHandshakeFailedSessionErrorCode)
		return QStringLiteral("无法连接本地 Windows Terminal");
	if (error.code == TerminalCommandTimeoutSessionErrorCode)
		return QStringLiteral("终端控制命令超时，已安全关闭终端");
	if (error.code == IdentityUnavailableSessionErrorCode)
		return QStringLiteral("本机设备身份不可用，请检查 security 目录");
	if (error.code == AuthenticationTimeoutSessionErrorCode)
		return QStringLiteral("设备身份认证超时");
	if (error.code == SignatureInvalidSessionErrorCode)
		return QStringLiteral("设备签名验证失败，已终止连接");
	if (error.code == PairingRejectedSessionErrorCode)
		return QStringLiteral("设备配对已被拒绝");
	if (error.code == PairingRateLimitedSessionErrorCode)
		return QStringLiteral("配对失败次数过多，请稍后重试");
	if (error.code == DeviceKeyChangedSessionErrorCode)
		return QStringLiteral("已配对设备的密钥已变更，请先撤销旧记录");
	if (error.code == DeviceRevokedSessionErrorCode)
		return QStringLiteral("该设备的信任已被撤销");
	if (error.code == PermissionDeniedSessionErrorCode)
		return QStringLiteral("当前会话没有执行此操作的权限");
	if (error.code == TrustStoreTamperedSessionErrorCode)
		return QStringLiteral("可信设备记录校验失败，身份认证已停用");
	if (error.code == InvalidArgumentSessionErrorCode)
		return QStringLiteral("连接参数无效");
	if (error.domain == ProtocolSessionErrorDomain)
		return QStringLiteral("收到无效或不兼容的协议消息");
	return QStringLiteral("远程会话发生错误");
}
