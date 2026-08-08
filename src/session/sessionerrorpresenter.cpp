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
	if (error.code == CaptureFailedSessionErrorCode)
		return QStringLiteral("屏幕采集失败");
	if (error.code == InvalidArgumentSessionErrorCode)
		return QStringLiteral("连接参数无效");
	if (error.domain == ProtocolSessionErrorDomain)
		return QStringLiteral("收到无效或不兼容的协议消息");
	return QStringLiteral("远程会话发生错误");
}
