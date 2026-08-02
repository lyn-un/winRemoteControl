#include "ui_bridge/webviewwidget.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include <wrl/event.h>

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

void KWebViewWidget::loadLocalFile(const QString &strFilePath, const QString &strViewMode)
{
	const QFileInfo fileInfo(strFilePath);
	m_strFrontendFolder = fileInfo.absolutePath();
	m_strPendingUrl = QStringLiteral("https://winremotecontrol.local/index.html?view=%1").arg(strViewMode);
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
	postJson(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebViewWidget::sendApplicationSettingsError(const QString &strError)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("applicationSettingsError"));
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
	else if (strCommand == QStringLiteral("respondIncomingAccessRequest"))
	{
		const QJsonObject object = document.object();
		emit respondIncomingAccessRequestRequested(
			object.value(QStringLiteral("requestId")).toString(),
			object.value(QStringLiteral("accepted")).toBool(false));
	}
	else if (strCommand == QStringLiteral("disconnectSession"))
		emit disconnectSessionRequested();
	else if (strCommand == QStringLiteral("startStreaming"))
		emit startStreamingRequested();
	else if (strCommand == QStringLiteral("stopStreaming"))
		emit stopStreamingRequested();
	else if (strCommand == QStringLiteral("enterDesktop"))
		emit enterDesktopRequested();
	else if (strCommand == QStringLiteral("closeDesktop"))
		emit closeDesktopRequested();
	else if (strCommand == QStringLiteral("minimizeDesktopWindow"))
		emit minimizeDesktopWindowRequested();
	else if (strCommand == QStringLiteral("toggleMaximizeDesktopWindow"))
		emit toggleMaximizeDesktopWindowRequested();
	else if (strCommand == QStringLiteral("beginDesktopWindowDrag"))
		emit beginDesktopWindowDragRequested();
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
