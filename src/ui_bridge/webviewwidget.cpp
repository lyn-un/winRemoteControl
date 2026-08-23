#include "ui_bridge/webviewwidget.h"

#include "core/protocol/filetransfercontrolmessage.h"
#include "session/sessionerrorpresenter.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <wrl/event.h>

namespace
{
	QString FileTransferPaneName(KFileTransferPane pane)
	{
		return pane == RemoteFileTransferPane
			? QStringLiteral("remote") : QStringLiteral("local");
	}

	QString FileTransferEntryKind(KFileListingEntryType type)
	{
		switch (type)
		{
		case DriveFileListingEntryType:
			return QStringLiteral("drive");
		case DirectoryFileListingEntryType:
			return QStringLiteral("directory");
		case RegularFileListingEntryType:
			return QStringLiteral("file");
		default:
			return QStringLiteral("unknown");
		}
	}

	QString FileTransferDirectionNameForBridge(KFileTransferDirection direction)
	{
		return direction == DownloadFileTransferDirection
			? QStringLiteral("download") : QStringLiteral("upload");
	}

	QString FileTransferStatusText(const QString &strStatus)
	{
		if (strStatus == QStringLiteral("closed"))
			return QStringLiteral("文件传输未打开");
		if (strStatus == QStringLiteral("opening"))
			return QStringLiteral("正在请求文件传输");
		if (strStatus == QStringLiteral("waiting_channels"))
			return QStringLiteral("正在建立安全文件通道");
		if (strStatus == QStringLiteral("ready"))
			return QStringLiteral("文件传输已就绪");
		if (strStatus == QStringLiteral("reconnecting"))
			return QStringLiteral("网络恢复中，传输已暂停");
		if (strStatus == QStringLiteral("closing"))
			return QStringLiteral("正在停止文件传输");
		if (strStatus == QStringLiteral("unavailable")
			|| strStatus == QStringLiteral("permission_denied"))
		{
			return QStringLiteral("当前可信设备未授予文件传输权限");
		}
		if (strStatus == QStringLiteral("stopped_by_controlled"))
			return QStringLiteral("被控端已停止文件传输");
		if (strStatus == QStringLiteral("stopped_by_remote"))
			return QStringLiteral("对方已停止文件传输");
		if (strStatus == QStringLiteral("channel_closed"))
			return QStringLiteral("文件传输通道已关闭");
		return strStatus;
	}

	QString FileTransferErrorText(const QString &strErrorCode)
	{
		if (strErrorCode == QStringLiteral("invalid_pane")
			|| strErrorCode == QStringLiteral("invalid_direction")
			|| strErrorCode == QStringLiteral("invalid_copy_request"))
		{
			return QStringLiteral("文件传输请求无效，请刷新后重试");
		}
		if (strErrorCode == QStringLiteral("stale_listing")
			|| strErrorCode == QStringLiteral("stale_page_token")
			|| strErrorCode == QStringLiteral("stale_source")
			|| strErrorCode == QStringLiteral("stale_destination"))
		{
			return QStringLiteral("目录内容已经变化，请刷新后重新选择");
		}
		if (strErrorCode == QStringLiteral("source_changed"))
			return QStringLiteral("源文件已经变化，请重新发起传输");
		if (strErrorCode == QStringLiteral("permission_denied")
			|| strErrorCode == QStringLiteral("role_not_allowed")
			|| strErrorCode == QStringLiteral("controller_required"))
		{
			return QStringLiteral("当前会话没有执行此文件操作的权限");
		}
		if (strErrorCode == QStringLiteral("file_transfer_unavailable")
			|| strErrorCode == QStringLiteral("unsupported_mode"))
		{
			return QStringLiteral("当前设备不支持文件传输或尚未授权");
		}
		if (strErrorCode == QStringLiteral("open_timeout")
			|| strErrorCode == QStringLiteral("channel_closed")
			|| strErrorCode == QStringLiteral("channel_create_failed"))
		{
			return QStringLiteral("文件传输通道不可用，请稍后重新打开");
		}
		if (strErrorCode == QStringLiteral("list_failed")
			|| strErrorCode == QStringLiteral("destination_unavailable")
			|| strErrorCode == QStringLiteral("destination_open_failed"))
		{
			return QStringLiteral("无法访问所选位置，请检查权限和磁盘状态");
		}
		if (strErrorCode == QStringLiteral("integrity_check_failed"))
			return QStringLiteral("文件完整性校验失败，未写入目标文件");
		if (strErrorCode == QStringLiteral("write_failed"))
			return QStringLiteral("写入文件失败，请检查磁盘空间和目录权限");
		if (strErrorCode == QStringLiteral("cancelled"))
			return QStringLiteral("文件传输已取消");
		if (strErrorCode.isEmpty())
			return QStringLiteral("文件传输发生错误");
		return QStringLiteral("文件传输失败（%1）").arg(strErrorCode);
	}

	QJsonObject FileTransferTaskObject(const KFileTransferTaskSnapshot &task)
	{
		QJsonObject object;
		object.insert(QStringLiteral("taskId"), task.strTaskId);
		object.insert(QStringLiteral("fileId"), task.strFileId);
		object.insert(QStringLiteral("displayName"), task.strDisplayName);
		object.insert(QStringLiteral("kind"), FileTransferTaskKindName(task.kind));
		object.insert(QStringLiteral("direction"),
			FileTransferDirectionNameForBridge(task.direction));
		object.insert(QStringLiteral("status"), FileTransferTaskStateName(task.state));
		object.insert(QStringLiteral("bytesTransferred"),
			QString::number(task.nBytesTransferred));
		object.insert(QStringLiteral("bytesTotal"), QString::number(task.nBytesTotal));
		object.insert(QStringLiteral("errorCode"), task.strErrorCode);
		object.insert(QStringLiteral("canPause"), task.bCanPause);
		object.insert(QStringLiteral("canRetry"), task.bCanRetry);
		return object;
	}
}

KWebViewWidget::KWebViewWidget(QWidget *pParent)
	: QWidget(pParent)
{
	setAttribute(Qt::WA_NativeWindow, true);
	setAttribute(Qt::WA_DontCreateNativeAncestors, true);
}

KWebViewWidget::~KWebViewWidget()
{
	if (m_spController)
		m_spController->Close();
}

void KWebViewWidget::loadLocalFile(const QString &strFilePath,
	const QString &strViewMode,
	const QString &strThemeId)
{
	const QFileInfo fileInfo(strFilePath);
	m_strFrontendFolder = fileInfo.absolutePath();
	QUrl url(QStringLiteral("https://winremotecontrol.local/index.html"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("view"), strViewMode);
	query.addQueryItem(QStringLiteral("theme"), strThemeId);
	url.setQuery(query);
	m_strPendingUrl = url.toString();
	navigateIfReady();
}

void KWebViewWidget::sendStatusChanged(const QString &strStatus)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("statusChanged"));
	object.insert(QStringLiteral("status"), strStatus);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendSignalingChanged(const QString &strState)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("signalingChanged"));
	object.insert(QStringLiteral("state"), strState);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendWebRtcStateChanged(const QString &strState)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("webrtcStateChanged"));
	object.insert(QStringLiteral("state"), strState);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendSessionChannelChanged(bool bOpen)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("sessionChannelChanged"));
	object.insert(QStringLiteral("open"), bOpen);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendDeviceInfoChanged(const QString &strComputerName,
	const QString &strWallpaperMime,
	const QString &strWallpaperData,
	int nScreenWidth,
	int nScreenHeight)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("deviceInfoChanged"));
	object.insert(QStringLiteral("computerName"), strComputerName);
	object.insert(QStringLiteral("wallpaperMime"), strWallpaperMime);
	object.insert(QStringLiteral("wallpaperData"), strWallpaperData);
	object.insert(QStringLiteral("screenWidth"), nScreenWidth);
	object.insert(QStringLiteral("screenHeight"), nScreenHeight);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendCaptureError(const QString &strMessage)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("captureError"));
	object.insert(QStringLiteral("message"), strMessage);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFrameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("frameReady"));
	object.insert(QStringLiteral("width"), nWidth);
	object.insert(QStringLiteral("height"), nHeight);
	object.insert(QStringLiteral("frameIndex"), QString::number(nFrameIndex));
	object.insert(QStringLiteral("timestampMs"), QString::number(nTimestampMs));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendNetworkStatsChanged(const KNetworkStats &stats)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("networkStatsChanged"));
	object.insert(QStringLiteral("quality"), stats.strQuality);
	object.insert(QStringLiteral("rttMs"), stats.nRttMs);
	object.insert(QStringLiteral("jitterMs"), stats.nJitterMs);
	object.insert(QStringLiteral("packetLossRate"), stats.fPacketLossRate);
	object.insert(QStringLiteral("bitrateKbps"), stats.nBitrateKbps);
	object.insert(QStringLiteral("fps"), stats.nFps);
	object.insert(QStringLiteral("dataChannelRttMs"), stats.nDataChannelRttMs);
	object.insert(QStringLiteral("jitterBufferDelayMs"), stats.nJitterBufferDelayMs);
	object.insert(QStringLiteral("jitterBufferTargetDelayMs"), stats.nJitterBufferTargetDelayMs);
	object.insert(QStringLiteral("decodeTimeMs"), stats.nDecodeTimeMs);
	object.insert(QStringLiteral("framesDecoded"), stats.nFramesDecoded);
	object.insert(QStringLiteral("keyFramesDecoded"), stats.nKeyFramesDecoded);
	object.insert(QStringLiteral("framesDropped"), stats.nFramesDropped);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendSessionError(const KSessionError &error)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("sessionError"));
	object.insert(QStringLiteral("domain"), KSessionError::domainName(error.domain));
	object.insert(QStringLiteral("code"), KSessionError::codeName(error.code));
	object.insert(QStringLiteral("stage"), KSessionError::stageName(error.stage));
	object.insert(QStringLiteral("retryable"), error.bRetryable);
	object.insert(QStringLiteral("message"), KSessionErrorPresenter::userMessage(error));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendLanDevicesChanged(const QVector<KDiscoveredDevice> &devices)
{
	QJsonArray deviceArray;
	for (const KDiscoveredDevice &device : devices)
	{
		QJsonObject deviceObject;
		deviceObject.insert(QStringLiteral("deviceId"), device.strDeviceId);
		deviceObject.insert(QStringLiteral("name"), device.strDeviceName);
		deviceObject.insert(QStringLiteral("address"), device.strHost);
		deviceObject.insert(QStringLiteral("port"), device.nSignalingPort);
		deviceObject.insert(QStringLiteral("online"), true);
		deviceArray.append(deviceObject);
	}
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("lanDevicesChanged"));
	object.insert(QStringLiteral("devices"), deviceArray);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendLanDiscoveryError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("lanDiscoveryError"));
	object.insert(QStringLiteral("message"), strError);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendRecentDevicesChanged(const QVector<KRecentDevice> &devices)
{
	QJsonArray deviceArray;
	for (const KRecentDevice &device : devices)
	{
		QJsonObject deviceObject;
		deviceObject.insert(QStringLiteral("deviceId"), device.strDeviceId);
		deviceObject.insert(QStringLiteral("authenticatedDeviceId"),
			device.strAuthenticatedDeviceId);
		deviceObject.insert(QStringLiteral("name"), device.strDeviceName);
		deviceObject.insert(QStringLiteral("host"), device.strHost);
		deviceObject.insert(QStringLiteral("port"), device.nSignalingPort);
		deviceObject.insert(QStringLiteral("lastConnectedAtMs"),
			QString::number(device.nLastConnectedAtMs));
		deviceObject.insert(QStringLiteral("incoming"), device.bIncoming);
		deviceArray.append(deviceObject);
	}
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("recentDevicesChanged"));
	object.insert(QStringLiteral("devices"), deviceArray);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendRecentDeviceError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("recentDeviceError"));
	object.insert(QStringLiteral("message"), strError);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendApplicationSettingsChanged(const KApplicationSettings &settings)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("applicationSettingsChanged"));
	object.insert(QStringLiteral("remoteAccessEnabled"), settings.bRemoteAccessEnabled);
	object.insert(QStringLiteral("approvalMode"), RemoteApprovalModeName(settings.approvalMode));
	object.insert(QStringLiteral("approvalTimeoutSeconds"), settings.nApprovalTimeoutSeconds);
	object.insert(QStringLiteral("defaultListenPort"), settings.nDefaultListenPort);
	object.insert(QStringLiteral("themeId"), settings.strThemeId);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendApplicationSettingsError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("applicationSettingsError"));
	object.insert(QStringLiteral("message"), strError);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendApplicationThemeError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("applicationThemeError"));
	object.insert(QStringLiteral("message"), strError);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendIncomingAccessRequest(const QString &strRequestId,
	const QString &strDeviceName,
	const QString &strSourceAddress,
	qint64 nExpiresAtMs)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("incomingAccessRequest"));
	object.insert(QStringLiteral("requestId"), strRequestId);
	object.insert(QStringLiteral("deviceName"), strDeviceName);
	object.insert(QStringLiteral("sourceAddress"), strSourceAddress);
	object.insert(QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendIncomingAccessRequestCleared(
	const QString &strRequestId,
	const QString &strReason)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("incomingAccessRequestCleared"));
	object.insert(QStringLiteral("requestId"), strRequestId);
	object.insert(QStringLiteral("reason"), strReason);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendPairingRequest(const QString &strRequestId,
	const QString &strDeviceName,
	const QString &strLocalRole,
	const QString &strVerificationCode,
	const QString &strControllerFingerprint,
	const QString &strControlledFingerprint,
	const QString &strTlsProtocol,
	const QString &strCipherSuite,
	KPermissionScopes requestedPermissions,
	qint64 nExpiresAtMs)
{
	QJsonArray permissions;
	for (const QString &strName : PermissionScopeNames(requestedPermissions))
		permissions.append(strName);
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("pairingRequestChanged"));
	object.insert(QStringLiteral("requestId"), strRequestId);
	object.insert(QStringLiteral("deviceName"), strDeviceName);
	object.insert(QStringLiteral("localRole"), strLocalRole);
	object.insert(QStringLiteral("verificationCode"), strVerificationCode);
	object.insert(QStringLiteral("controllerFingerprint"), strControllerFingerprint);
	object.insert(QStringLiteral("controlledFingerprint"), strControlledFingerprint);
	object.insert(QStringLiteral("tlsProtocol"), strTlsProtocol);
	object.insert(QStringLiteral("cipherSuite"), strCipherSuite);
	object.insert(QStringLiteral("permissions"), permissions);
	object.insert(QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendPairingCleared(const QString &strRequestId,
	const QString &strReason)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("pairingRequestCleared"));
	object.insert(QStringLiteral("requestId"), strRequestId);
	object.insert(QStringLiteral("reason"), strReason);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendDeviceAuthenticationStateChanged(
	const QString &strState,
	const QString &strDeviceId,
	const QString &strFingerprint,
	bool bTrusted)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("deviceAuthenticationStateChanged"));
	object.insert(QStringLiteral("state"), strState);
	object.insert(QStringLiteral("deviceId"), strDeviceId);
	object.insert(QStringLiteral("fingerprint"), strFingerprint);
	object.insert(QStringLiteral("trusted"), bTrusted);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendTrustedDevicesChanged(
	const QVector<KTrustedDevice> &devices)
{
	QJsonArray items;
	for (const KTrustedDevice &device : devices)
	{
		QJsonObject item;
		item.insert(QStringLiteral("deviceId"), device.strDeviceId);
		item.insert(QStringLiteral("name"), device.strAlias.isEmpty()
			? device.strAdvertisedName : device.strAlias);
		item.insert(QStringLiteral("fingerprint"), device.strFingerprint.left(12));
		item.insert(QStringLiteral("permissions"), QJsonArray::fromStringList(
			PermissionScopeNames(device.permissionLimit)));
		item.insert(QStringLiteral("pairedAtMs"), QString::number(device.nPairedAtMs));
		item.insert(QStringLiteral("lastAuthenticatedAtMs"),
			QString::number(device.nLastAuthenticatedAtMs));
		item.insert(QStringLiteral("revoked"), device.bRevoked);
		items.append(item);
	}
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("trustedDevicesChanged"));
	object.insert(QStringLiteral("devices"), items);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendTrustedDeviceError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("trustedDeviceError"));
	object.insert(QStringLiteral("message"), strError);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendSecurityMigrationNotice(const QString &strMessage)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("securityMigrationNotice"));
	object.insert(QStringLiteral("message"), strMessage);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendSessionPermissionsChanged(KPermissionScopes permissions)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("sessionPermissionsChanged"));
	object.insert(QStringLiteral("permissions"), QJsonArray::fromStringList(
		PermissionScopeNames(permissions)));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendClipboardSyncStateChanged(bool bEnabled,
	bool bAvailable,
	bool bActive,
	const QString &strStatus)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("clipboardSyncStateChanged"));
	object.insert(QStringLiteral("enabled"), bEnabled);
	object.insert(QStringLiteral("available"), bAvailable);
	object.insert(QStringLiteral("active"), bActive);
	object.insert(QStringLiteral("status"), strStatus);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendClipboardSyncError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("clipboardSyncError"));
	object.insert(QStringLiteral("message"), strError);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendSessionCapabilitiesChanged(
	const KNegotiatedCapabilities &capabilities)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("sessionCapabilitiesChanged"));
	object.insert(QStringLiteral("available"), capabilities.bValid);
	object.insert(QStringLiteral("protocolVersion"), capabilities.nProtocolVersion);
	object.insert(QStringLiteral("videoCodec"), capabilities.strVideoCodec);
	object.insert(QStringLiteral("maximumWidth"), capabilities.nMaximumWidth);
	object.insert(QStringLiteral("maximumHeight"), capabilities.nMaximumHeight);
	object.insert(QStringLiteral("maximumFps"), capabilities.nMaximumFps);
	object.insert(QStringLiteral("maximumBitrateKbps"), capabilities.nMaximumBitrateKbps);
	object.insert(QStringLiteral("clipboardText"), capabilities.bClipboardText);
	object.insert(QStringLiteral("keyboard"), capabilities.bKeyboard);
	object.insert(QStringLiteral("unicodeText"), capabilities.bUnicodeText);
	object.insert(QStringLiteral("mouseButtons"), capabilities.bMouseButtons);
	object.insert(QStringLiteral("mouseWheel"), capabilities.bMouseWheel);
	object.insert(QStringLiteral("fileTransfer"), capabilities.bFileTransfer);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendTerminalStateChanged(
	KTerminalState state,
	bool bAvailable,
	const QString &strStatus,
	const QString &strDeviceName,
	const QString &strDeviceSource)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("terminalStateChanged"));
	object.insert(QStringLiteral("state"), TerminalStateName(state));
	object.insert(QStringLiteral("available"), bAvailable);
	object.insert(QStringLiteral("status"), strStatus);
	object.insert(QStringLiteral("deviceName"), strDeviceName);
	object.insert(QStringLiteral("deviceSource"), strDeviceSource);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendTerminalFrontendSupportChanged(
	bool bSupported,
	const QString &strReason)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("terminalFrontendSupportChanged"));
	object.insert(QStringLiteral("supported"), bSupported);
	object.insert(QStringLiteral("reason"), strReason);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendIncomingTerminalRequest(
	const QString &strRequestId,
	const QString &strDeviceName,
	const QString &strDeviceSource,
	qint64 nExpiresAtMs)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("incomingTerminalAccessRequest"));
	object.insert(QStringLiteral("requestId"), strRequestId);
	object.insert(QStringLiteral("deviceName"), strDeviceName);
	object.insert(QStringLiteral("sourceAddress"), strDeviceSource);
	object.insert(QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendIncomingTerminalRequestCleared(
	const QString &strRequestId,
	const QString &strReason)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("incomingTerminalAccessRequestCleared"));
	object.insert(QStringLiteral("requestId"), strRequestId);
	object.insert(QStringLiteral("reason"), strReason);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendTerminalError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("terminalError"));
	object.insert(QStringLiteral("message"), strError);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFileTransferStateChanged(
	KFileTransferState state,
	bool bAvailable,
	const QString &strStatus,
	const QString &strDeviceName,
	const QString &strDeviceSource,
	int nActiveTaskCount,
	quint64 nGeneration)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("fileTransferStateChanged"));
	object.insert(QStringLiteral("state"), FileTransferStateName(state));
	object.insert(QStringLiteral("available"), bAvailable);
	object.insert(QStringLiteral("status"), FileTransferStatusText(strStatus));
	object.insert(QStringLiteral("deviceName"), strDeviceName);
	object.insert(QStringLiteral("deviceSource"), strDeviceSource);
	object.insert(QStringLiteral("activeTasks"), nActiveTaskCount);
	object.insert(QStringLiteral("generation"), QString::number(nGeneration));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFilePaneLoading(
	KFileTransferPane pane,
	const QString &strRequestId)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("filePaneLoading"));
	object.insert(QStringLiteral("pane"), FileTransferPaneName(pane));
	object.insert(QStringLiteral("requestId"), strRequestId);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFilePaneChanged(const KFileTransferPaneSnapshot &snapshot)
{
	QJsonArray entries;
	for (const KFileTransferPaneEntry &entry : snapshot.entryList)
	{
		QJsonObject item;
		item.insert(QStringLiteral("entryId"), entry.strEntryId);
		item.insert(QStringLiteral("name"), entry.strName);
		item.insert(QStringLiteral("kind"), FileTransferEntryKind(entry.type));
		item.insert(QStringLiteral("sizeBytes"), QString::number(entry.nSize));
		item.insert(QStringLiteral("modifiedAtMs"),
			QString::number(entry.lastModifiedUtc.toMSecsSinceEpoch()));
		item.insert(QStringLiteral("navigable"), entry.bNavigable);
		item.insert(QStringLiteral("transferable"), entry.bTransferable);
		item.insert(QStringLiteral("extension"),
			entry.type == RegularFileListingEntryType
				? QFileInfo(entry.strName).suffix().toLower() : QString());
		entries.append(item);
	}

	QJsonObject location;
	location.insert(QStringLiteral("displayPath"), snapshot.strDisplayPath);
	location.insert(QStringLiteral("canGoUp"), snapshot.bCanGoUp);

	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("filePaneChanged"));
	object.insert(QStringLiteral("pane"), FileTransferPaneName(snapshot.pane));
	object.insert(QStringLiteral("requestId"), snapshot.strRequestId);
	object.insert(QStringLiteral("listingId"), snapshot.strListingId);
	object.insert(QStringLiteral("location"), location);
	object.insert(QStringLiteral("entries"), entries);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFileTransferSnapshot(
	const QVector<KFileTransferTaskSnapshot> &taskList)
{
	QJsonArray tasks;
	for (const KFileTransferTaskSnapshot &task : taskList)
		tasks.append(FileTransferTaskObject(task));
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("fileTransferSnapshot"));
	object.insert(QStringLiteral("tasks"), tasks);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFileTransferTaskChanged(
	const KFileTransferTaskSnapshot &task)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("fileTransferTaskChanged"));
	object.insert(QStringLiteral("task"), FileTransferTaskObject(task));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFileTransferTaskRemoved(const QString &strTaskId)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("fileTransferTaskRemoved"));
	object.insert(QStringLiteral("taskId"), strTaskId);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFileTransferConflictRequested(
	const KFileTransferConflictSnapshot &conflict)
{
	QJsonObject source;
	source.insert(QStringLiteral("sizeBytes"), QString::number(conflict.nSourceSize));
	source.insert(QStringLiteral("modifiedAtMs"),
		QString::number(conflict.sourceLastModifiedUtc.toMSecsSinceEpoch()));
	QJsonObject destination;
	destination.insert(QStringLiteral("sizeBytes"),
		QString::number(conflict.nDestinationSize));
	destination.insert(QStringLiteral("modifiedAtMs"),
		QString::number(conflict.destinationLastModifiedUtc.toMSecsSinceEpoch()));
	QJsonObject object;
	object.insert(QStringLiteral("type"),
		QStringLiteral("fileTransferConflictRequested"));
	object.insert(QStringLiteral("conflictId"), conflict.strConflictId);
	object.insert(QStringLiteral("taskId"), conflict.strTaskId);
	object.insert(QStringLiteral("fileId"), conflict.strFileId);
	object.insert(QStringLiteral("name"), conflict.strName);
	object.insert(QStringLiteral("source"), source);
	object.insert(QStringLiteral("destination"), destination);
	object.insert(QStringLiteral("applyToRemainingAllowed"),
		conflict.bApplyToRemainingAllowed);
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFileTransferError(
	const QString &strErrorCode,
	const QString &strMessage)
{
	Q_UNUSED(strMessage);
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("fileTransferError"));
	object.insert(QStringLiteral("code"), strErrorCode);
	object.insert(QStringLiteral("message"), FileTransferErrorText(strErrorCode));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendFileTransferClosePromptRequested()
{
	QJsonObject object;
	object.insert(QStringLiteral("type"),
		QStringLiteral("fileTransferClosePromptRequested"));
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::resizeEvent(QResizeEvent *pEvent)
{
	QWidget::resizeEvent(pEvent);
	resizeWebView();
}

void KWebViewWidget::showEvent(QShowEvent *pEvent)
{
	QWidget::showEvent(pEvent);
	initializeWebView();
}

void KWebViewWidget::initializeWebView()
{
	if (m_bInitializing || m_spWebView)
		return;

	m_bInitializing = true;
	HWND hwndParent = reinterpret_cast<HWND>(winId());
	HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(nullptr,
		nullptr,
		nullptr,
		Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[this, hwndParent](HRESULT result, ICoreWebView2Environment *pEnvironment) -> HRESULT
			{
				m_bInitializing = false;
				if (FAILED(result) || pEnvironment == nullptr)
				{
					sendCaptureError(QStringLiteral("Create WebView2 environment failed"));
					return S_OK;
				}

				m_spEnvironment = pEnvironment;
				m_spEnvironment->CreateCoreWebView2Controller(hwndParent,
					Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						[this](HRESULT controllerResult, ICoreWebView2Controller *pController) -> HRESULT
						{
							if (FAILED(controllerResult) || pController == nullptr)
							{
								sendCaptureError(QStringLiteral("Create WebView2 controller failed"));
								return S_OK;
							}

							m_spController = pController;
							m_spController->get_CoreWebView2(&m_spWebView);
							Microsoft::WRL::ComPtr<ICoreWebView2Controller3> spController3;
							if (SUCCEEDED(m_spController.As(&spController3)) && spController3)
							{
								spController3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
								spController3->put_ShouldDetectMonitorScaleChanges(TRUE);
							}
							resizeWebView();

							EventRegistrationToken token = {};
							m_spWebView->add_WebMessageReceived(
								Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
									[this](ICoreWebView2 *,
										ICoreWebView2WebMessageReceivedEventArgs *pArgs) -> HRESULT
									{
										LPWSTR pMessage = nullptr;
										if (SUCCEEDED(pArgs->get_WebMessageAsJson(&pMessage)) && pMessage != nullptr)
										{
											handleWebMessage(QString::fromWCharArray(pMessage));
											::CoTaskMemFree(pMessage);
										}
										return S_OK;
									}).Get(),
								&token);

							m_bNavigationReady = true;
							navigateIfReady();
							flushPendingMessages();
							return S_OK;
						}).Get());
				return S_OK;
			}).Get());

	if (FAILED(hr))
	{
		m_bInitializing = false;
		sendCaptureError(QStringLiteral("CreateCoreWebView2EnvironmentWithOptions failed"));
	}
}

void KWebViewWidget::resizeWebView()
{
	if (!m_spController)
		return;

	const QRect rect = this->rect();
	const double fDevicePixelRatio = devicePixelRatioF();
	RECT bounds = {
		static_cast<LONG>(rect.left() * fDevicePixelRatio),
		static_cast<LONG>(rect.top() * fDevicePixelRatio),
		static_cast<LONG>((rect.right() + 1) * fDevicePixelRatio),
		static_cast<LONG>((rect.bottom() + 1) * fDevicePixelRatio)
	};
	m_spController->put_Bounds(bounds);
}

void KWebViewWidget::navigateIfReady()
{
	if (!m_bNavigationReady || !m_spWebView || m_strPendingUrl.isEmpty())
		return;

	Microsoft::WRL::ComPtr<ICoreWebView2_3> spWebView3;
	const HRESULT hrQuery = m_spWebView.As(&spWebView3);
	if (SUCCEEDED(hrQuery) && spWebView3)
	{
		const std::wstring strHost = L"winremotecontrol.local";
		const std::wstring strFolder = toWideString(QDir::toNativeSeparators(m_strFrontendFolder));
		spWebView3->SetVirtualHostNameToFolderMapping(strHost.c_str(),
			strFolder.c_str(),
			COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
	}

	const std::wstring strUrl = toWideString(m_strPendingUrl);
	m_spWebView->Navigate(strUrl.c_str());
}

void KWebViewWidget::handleWebMessage(const QString &strMessage)
{
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	if (!document.isObject())
		return;

	const QString strCommand = document.object().value(QStringLiteral("command")).toString();
	if (strCommand == QStringLiteral("startCapture"))
		emit startCaptureRequested();
	else if (strCommand == QStringLiteral("stopCapture"))
		emit stopCaptureRequested();
	else if (strCommand == QStringLiteral("setRole"))
		emit setRoleRequested(document.object().value(QStringLiteral("role")).toString());
	else if (strCommand == QStringLiteral("startSignalingServer"))
		emit startSignalingServerRequested(static_cast<quint16>(document.object().value(QStringLiteral("port")).toInt(39000)));
	else if (strCommand == QStringLiteral("connectSignaling"))
		emit connectSignalingRequested(document.object().value(QStringLiteral("host")).toString(),
			static_cast<quint16>(document.object().value(QStringLiteral("port")).toInt(39000)));
	else if (strCommand == QStringLiteral("retryLastConnection"))
		emit retryLastConnectionRequested();
	else if (strCommand == QStringLiteral("setClipboardSyncEnabled"))
		emit setClipboardSyncEnabledRequested(
			document.object().value(QStringLiteral("enabled")).toBool(true));
	else if (strCommand == QStringLiteral("requestClipboardSyncState"))
		emit requestClipboardSyncStateRequested();
	else if (strCommand == QStringLiteral("refreshLanDevices"))
		emit refreshLanDevicesRequested();
	else if (strCommand == QStringLiteral("connectLanDevice"))
		emit connectLanDeviceRequested(
			document.object().value(QStringLiteral("deviceId")).toString());
	else if (strCommand == QStringLiteral("requestRecentDevices"))
		emit requestRecentDevicesRequested();
	else if (strCommand == QStringLiteral("connectRecentDevice"))
		emit connectRecentDeviceRequested(
			document.object().value(QStringLiteral("deviceId")).toString());
	else if (strCommand == QStringLiteral("removeRecentDevice"))
		emit removeRecentDeviceRequested(
			document.object().value(QStringLiteral("deviceId")).toString());
	else if (strCommand == QStringLiteral("openRecentDeviceTerminal"))
		emit openRecentDeviceTerminalRequested(
			document.object().value(QStringLiteral("deviceId")).toString());
	else if (strCommand == QStringLiteral("openCurrentTerminal"))
		emit openCurrentTerminalRequested();
	else if (strCommand == QStringLiteral("openCurrentFileTransfer"))
		emit openCurrentFileTransferRequested();
	else if (strCommand == QStringLiteral("stopCurrentFileTransfer"))
		emit stopCurrentFileTransferRequested();
	else if (strCommand == QStringLiteral("requestTerminalFrontendSupport"))
		emit requestTerminalFrontendSupportRequested();
	else if (strCommand == QStringLiteral("respondTerminalAccessRequest"))
	{
		const QJsonObject object = document.object();
		emit respondTerminalAccessRequestRequested(
			object.value(QStringLiteral("requestId")).toString(),
			object.value(QStringLiteral("accepted")).toBool(false));
	}
	else if (strCommand == QStringLiteral("closeTerminal"))
		emit closeTerminalRequested();
	else if (strCommand == QStringLiteral("requestApplicationSettings"))
		emit requestApplicationSettingsRequested();
	else if (strCommand == QStringLiteral("updateApplicationSettings"))
	{
		const QJsonObject object = document.object();
		emit updateApplicationSettingsRequested(
			object.value(QStringLiteral("remoteAccessEnabled")).toBool(true),
			object.value(QStringLiteral("approvalMode")).toString(),
			object.value(QStringLiteral("approvalTimeoutSeconds")).toInt(30),
			object.value(QStringLiteral("defaultListenPort")).toInt(39000));
	}
	else if (strCommand == QStringLiteral("updateApplicationTheme"))
	{
		emit updateApplicationThemeRequested(
			document.object().value(QStringLiteral("themeId")).toString());
	}
	else if (strCommand == QStringLiteral("respondIncomingAccessRequest"))
	{
		const QJsonObject object = document.object();
		emit respondIncomingAccessRequestRequested(
			object.value(QStringLiteral("requestId")).toString(),
			object.value(QStringLiteral("accepted")).toBool(false));
	}
	else if (strCommand == QStringLiteral("respondPairingRequest"))
	{
		const QJsonObject object = document.object();
		QStringList names;
		for (const QJsonValue &value : object.value(QStringLiteral("permissions")).toArray())
		{
			if (value.isString())
				names.append(value.toString());
		}
		KPermissionScopes permissions;
		if (!PermissionScopesFromNames(names, &permissions))
			permissions = KPermissionScopes();
		emit respondPairingRequestRequested(
			object.value(QStringLiteral("requestId")).toString(),
			object.value(QStringLiteral("accepted")).toBool(false),
			permissions);
	}
	else if (strCommand == QStringLiteral("requestTrustedDevices"))
		emit requestTrustedDevicesRequested();
	else if (strCommand == QStringLiteral("updateTrustedDevice"))
	{
		const QJsonObject object = document.object();
		QStringList names;
		for (const QJsonValue &value : object.value(QStringLiteral("permissions")).toArray())
		{
			if (value.isString())
				names.append(value.toString());
		}
		KPermissionScopes permissions;
		if (!PermissionScopesFromNames(names, &permissions))
			permissions = KPermissionScopes();
		emit updateTrustedDeviceRequested(
			object.value(QStringLiteral("deviceId")).toString(),
			object.value(QStringLiteral("alias")).toString(), permissions);
	}
	else if (strCommand == QStringLiteral("revokeTrustedDevice"))
		emit revokeTrustedDeviceRequested(
			document.object().value(QStringLiteral("deviceId")).toString());
	else if (strCommand == QStringLiteral("requestRePairDevice"))
		emit requestRePairDeviceRequested(
			document.object().value(QStringLiteral("deviceId")).toString());
	else if (strCommand == QStringLiteral("disconnectSession"))
		emit disconnectSessionRequested();
	else if (strCommand == QStringLiteral("startStreaming"))
		emit startStreamingRequested();
	else if (strCommand == QStringLiteral("stopStreaming"))
		emit stopStreamingRequested();
	else if (strCommand == QStringLiteral("enterDesktop"))
		emit enterDesktopRequested();
	else if (strCommand == QStringLiteral("minimizeMainWindow"))
		emit minimizeMainWindowRequested();
	else if (strCommand == QStringLiteral("closeMainWindow"))
		emit closeMainWindowRequested();
	else if (strCommand == QStringLiteral("beginMainWindowDrag"))
		emit beginMainWindowDragRequested();
	else if (strCommand == QStringLiteral("closeDesktop"))
		emit closeDesktopRequested();
	else if (strCommand == QStringLiteral("minimizeDesktopWindow"))
		emit minimizeDesktopWindowRequested();
	else if (strCommand == QStringLiteral("toggleMaximizeDesktopWindow"))
		emit toggleMaximizeDesktopWindowRequested();
	else if (strCommand == QStringLiteral("beginDesktopWindowDrag"))
		emit beginDesktopWindowDragRequested();
	else if (strCommand == QStringLiteral("closeFileTransferWindow"))
		emit closeFileTransferWindowRequested();
	else if (strCommand == QStringLiteral("minimizeFileTransferWindow"))
		emit minimizeFileTransferWindowRequested();
	else if (strCommand == QStringLiteral("toggleMaximizeFileTransferWindow"))
		emit toggleMaximizeFileTransferWindowRequested();
	else if (strCommand == QStringLiteral("beginFileTransferWindowDrag"))
		emit beginFileTransferWindowDragRequested();
	else if (strCommand == QStringLiteral("requestFileTransferSnapshot"))
		emit requestFileTransferSnapshotRequested();
	else if (strCommand == QStringLiteral("navigateFilePane"))
	{
		const QJsonObject object = document.object();
		emit navigateFilePaneRequested(
			object.value(QStringLiteral("pane")).toString(),
			object.value(QStringLiteral("listingId")).toString(),
			object.value(QStringLiteral("targetEntryId")).toString());
	}
	else if (strCommand == QStringLiteral("navigateFilePaneByPath"))
	{
		const QJsonObject object = document.object();
		emit navigateFilePaneByPathRequested(
			object.value(QStringLiteral("pane")).toString(),
			object.value(QStringLiteral("path")).toString());
	}
	else if (strCommand == QStringLiteral("navigateFilePaneUp"))
	{
		const QJsonObject object = document.object();
		emit navigateFilePaneUpRequested(
			object.value(QStringLiteral("pane")).toString(),
			object.value(QStringLiteral("listingId")).toString());
	}
	else if (strCommand == QStringLiteral("refreshFilePane"))
	{
		emit refreshFilePaneRequested(
			document.object().value(QStringLiteral("pane")).toString());
	}
	else if (strCommand == QStringLiteral("startFileCopy"))
	{
		const QJsonObject object = document.object();
		const QJsonValue entryIdValue = object.value(
			QStringLiteral("sourceEntryIds"));
		QStringList entryIds;
		bool bValidEntryIds = entryIdValue.isArray();
		const QJsonArray entryIdArray = entryIdValue.toArray();
		if (entryIdArray.isEmpty()
			|| entryIdArray.size()
				> KFileTransferControlMessageCodec::kMaximumEntryIdCount)
		{
			bValidEntryIds = false;
		}
		for (const QJsonValue &value : entryIdArray)
		{
			const QString strEntryId = value.toString();
			if (!value.isString() || strEntryId.isEmpty()
				|| strEntryId.size() > 64)
			{
				bValidEntryIds = false;
				break;
			}
			entryIds.append(strEntryId);
		}
		const QString strSourceListingId = object.value(
			QStringLiteral("sourceListingId")).toString();
		const QString strDestinationListingId = object.value(
			QStringLiteral("destinationListingId")).toString();
		if (!bValidEntryIds || strSourceListingId.isEmpty()
			|| strSourceListingId.size() > 64
			|| strDestinationListingId.isEmpty()
			|| strDestinationListingId.size() > 64)
		{
			sendFileTransferError(QStringLiteral("invalid_copy_request"),
				QString());
			return;
		}
		emit startFileCopyRequested(
			object.value(QStringLiteral("sourcePane")).toString(),
			strSourceListingId,
			entryIds,
			strDestinationListingId);
	}
	else if (strCommand == QStringLiteral("pauseFileTransferTask"))
		emit pauseFileTransferTaskRequested(
			document.object().value(QStringLiteral("taskId")).toString());
	else if (strCommand == QStringLiteral("resumeFileTransferTask"))
		emit resumeFileTransferTaskRequested(
			document.object().value(QStringLiteral("taskId")).toString());
	else if (strCommand == QStringLiteral("cancelFileTransferTask"))
		emit cancelFileTransferTaskRequested(
			document.object().value(QStringLiteral("taskId")).toString());
	else if (strCommand == QStringLiteral("retryFileTransferTask"))
		emit retryFileTransferTaskRequested(
			document.object().value(QStringLiteral("taskId")).toString());
	else if (strCommand == QStringLiteral("resolveFileConflict"))
	{
		const QJsonObject object = document.object();
		emit resolveFileConflictRequested(
			object.value(QStringLiteral("conflictId")).toString(),
			object.value(QStringLiteral("resolution")).toString(),
			object.value(QStringLiteral("applyToRemaining")).toBool(false));
	}
	else if (strCommand == QStringLiteral("clearCompletedFileTransferTasks"))
		emit clearCompletedFileTransferTasksRequested();
	else if (strCommand == QStringLiteral("showControlCenterMenu"))
	{
		const QJsonObject object = document.object();
		const QPoint pos(object.value(QStringLiteral("x")).toInt(),
			object.value(QStringLiteral("y")).toInt());
		emit showControlCenterMenuRequested(pos);
	}
	else if (strCommand == QStringLiteral("setStreamConfig"))
	{
		const QJsonObject object = document.object();
		KStreamConfig config;
		config.nFps = object.value(QStringLiteral("fps")).toInt(config.nFps);
		config.nWidth = object.value(QStringLiteral("width")).toInt(config.nWidth);
		config.nHeight = object.value(QStringLiteral("height")).toInt(config.nHeight);
		config.nBitrateKbps = object.value(QStringLiteral("bitrateKbps")).toInt(config.nBitrateKbps);
		emit streamConfigRequested(config);
	}
	else if (strCommand == QStringLiteral("previewRectChanged"))
	{
		const QJsonObject object = document.object();
		const QRect rect(object.value(QStringLiteral("x")).toInt(),
			object.value(QStringLiteral("y")).toInt(),
			object.value(QStringLiteral("width")).toInt(),
			object.value(QStringLiteral("height")).toInt());
		emit previewRectChanged(rect);
	}
}

void KWebViewWidget::postJson(const QString &strJson)
{
	if (!m_spWebView)
	{
		m_pendingMessages.enqueue(strJson);
		return;
	}

	const std::wstring strMessage = toWideString(strJson);
	m_spWebView->PostWebMessageAsJson(strMessage.c_str());
}

void KWebViewWidget::flushPendingMessages()
{
	while (m_spWebView && !m_pendingMessages.isEmpty())
		postJson(m_pendingMessages.dequeue());
}

std::wstring KWebViewWidget::toWideString(const QString &strValue)
{
	return std::wstring(reinterpret_cast<const wchar_t *>(strValue.utf16()), static_cast<size_t>(strValue.size()));
}
