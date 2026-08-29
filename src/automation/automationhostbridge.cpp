#include "automation/automationhostbridge.h"

#include "commands/applicationcommandregistry.h"
#include "core/protocol/sessionmessage.h"
#include "core/session/sessionerror.h"
#include "core/session/sessionstatemachine.h"
#include "session/sessioncontroller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>

#include <cstring>

namespace
{
	constexpr int kMaximumAutomationEvents = 512;

	QByteArray BytesFromRaw(const char *pData, std::uint32_t nBytes)
	{
		if (pData == nullptr || nBytes == 0)
			return QByteArray();
		return QByteArray(pData, static_cast<qsizetype>(nBytes));
	}

	QString PrivacyModeName(KPrivacyMode mode)
	{
		if (mode == PrivacyOverlayPrivacyMode)
			return QStringLiteral("privacyOverlay");
		if (mode == DisplayOffPrivacyMode)
			return QStringLiteral("displayOff");
		return QStringLiteral("disabled");
	}

	QString PrivacyStateName(KPrivacyModeState state)
	{
		if (state == ApplyingPrivacyModeState)
			return QStringLiteral("applying");
		if (state == ActivePrivacyModeState)
			return QStringLiteral("active");
		if (state == RestoringPrivacyModeState)
			return QStringLiteral("restoring");
		if (state == FailedPrivacyModeState)
			return QStringLiteral("failed");
		return QStringLiteral("inactive");
	}

	QString PostSessionActionName(KPostSessionAction action)
	{
		return action == LockWorkstationPostSessionAction
			? QStringLiteral("lockWorkstation")
			: QStringLiteral("none");
	}

	QJsonObject ErrorObject(const QString &strErrorCode, const QString &strMessage)
	{
		return QJsonObject{
			{QStringLiteral("status"), static_cast<int>(ApplicationCommandFailed)},
			{QStringLiteral("value"), QJsonObject()},
			{QStringLiteral("errorCode"), strErrorCode},
			{QStringLiteral("technicalMessage"), strMessage}
		};
	}
}

KAutomationHostBridge::KAutomationHostBridge(KApplicationCommandRegistry *pRegistry,
	KSessionController *pSessionController,
	const QString &strDataDirectory,
	QObject *pParent)
	: QObject(pParent)
	, m_pRegistry(pRegistry)
	, m_pSessionController(pSessionController)
	, m_strDataDirectory(strDataDirectory)
	, m_nObservedSessionGeneration(pSessionController->sessionGeneration())
{
	Q_ASSERT(m_pRegistry != nullptr);
	Q_ASSERT(m_pSessionController != nullptr);
	m_hostApi.pHostContext = this;
	m_hostApi.submitCommand = &KAutomationHostBridge::SubmitCommand;
	m_hostApi.requestSnapshot = &KAutomationHostBridge::RequestSnapshot;
	m_hostApi.copyHostValue = &KAutomationHostBridge::CopyHostValue;
	m_hostApi.writeLog = &KAutomationHostBridge::WriteLog;
	initializeStateConnections();
}

KAutomationHostBridge::~KAutomationHostBridge()
{
	stopAcceptingRequests();
}

const KWrcDriverHostApiV1 *KAutomationHostBridge::hostApi() const
{
	return &m_hostApi;
}

void KAutomationHostBridge::stopAcceptingRequests()
{
	m_bAcceptingRequests = false;
}

void KAutomationHostBridge::SubmitCommand(void *pHostContext,
	std::uint64_t nRequestId,
	const char *pCommandIdUtf8,
	std::uint32_t nCommandIdBytes,
	const char *pArgumentsJsonUtf8,
	std::uint32_t nArgumentsJsonBytes,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	auto *pBridge = static_cast<KAutomationHostBridge *>(pHostContext);
	if (pBridge == nullptr || pCallback == nullptr)
		return;
	pBridge->submitCommand(nRequestId,
		BytesFromRaw(pCommandIdUtf8, nCommandIdBytes),
		BytesFromRaw(pArgumentsJsonUtf8, nArgumentsJsonBytes),
		pCallback, pCallbackContext);
}

void KAutomationHostBridge::RequestSnapshot(void *pHostContext,
	std::uint64_t nRequestId,
	const char *pSnapshotKindUtf8,
	std::uint32_t nSnapshotKindBytes,
	std::uint64_t nSinceSequence,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	auto *pBridge = static_cast<KAutomationHostBridge *>(pHostContext);
	if (pBridge == nullptr || pCallback == nullptr)
		return;
	pBridge->requestSnapshot(nRequestId,
		BytesFromRaw(pSnapshotKindUtf8, nSnapshotKindBytes),
		nSinceSequence, pCallback, pCallbackContext);
}

std::uint32_t KAutomationHostBridge::CopyHostValue(void *pHostContext,
	const char *pKeyUtf8,
	std::uint32_t nKeyBytes,
	char *pDestinationUtf8,
	std::uint32_t nDestinationBytes)
{
	const auto *pBridge = static_cast<const KAutomationHostBridge *>(pHostContext);
	if (pBridge == nullptr)
		return 0;
	const QString strKey = QString::fromUtf8(pKeyUtf8, static_cast<qsizetype>(nKeyBytes));
	QByteArray value;
	if (strKey == QStringLiteral("pid"))
		value = QByteArray::number(QCoreApplication::applicationPid());
	else if (strKey == QStringLiteral("buildId"))
		value = QByteArray(KWrcAutomationBuildId);
	else if (strKey == QStringLiteral("dataDirectory"))
		value = pBridge->m_strDataDirectory.toUtf8();
	else
		return 0;

	const std::uint32_t nRequired = static_cast<std::uint32_t>(value.size() + 1);
	if (pDestinationUtf8 == nullptr || nDestinationBytes < nRequired)
		return nRequired;
	std::memcpy(pDestinationUtf8, value.constData(), static_cast<size_t>(value.size()));
	pDestinationUtf8[value.size()] = '\0';
	return nRequired;
}

void KAutomationHostBridge::WriteLog(void *pHostContext,
	std::uint32_t nLevel,
	const char *pMessageUtf8,
	std::uint32_t nMessageBytes)
{
	if (pHostContext == nullptr)
		return;
	const QString strMessage = QString::fromUtf8(
		pMessageUtf8, static_cast<qsizetype>(nMessageBytes));
	if (nLevel >= 2)
		qWarning().noquote() << QStringLiteral("Automation driver: %1").arg(strMessage);
	else
		qInfo().noquote() << QStringLiteral("Automation driver: %1").arg(strMessage);
}

void KAutomationHostBridge::submitCommand(quint64 nRequestId,
	const QByteArray &commandIdUtf8,
	const QByteArray &argumentsJsonUtf8,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	const quint64 nSubmittedGeneration = m_nObservedSessionGeneration.load();
	QMetaObject::invokeMethod(this,
		[this, nRequestId, nSubmittedGeneration, commandIdUtf8, argumentsJsonUtf8,
			pCallback, pCallbackContext]()
		{
			if (!m_bAcceptingRequests)
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("application_shutdown"),
						QStringLiteral("Application is shutting down")),
					pCallback, pCallbackContext);
				return;
			}
			if (nSubmittedGeneration != m_pSessionController->sessionGeneration())
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("stale_generation"),
						QStringLiteral("Remote session changed before command execution")),
					pCallback, pCallbackContext);
				return;
			}

			QJsonParseError parseError;
			const QJsonDocument document = QJsonDocument::fromJson(argumentsJsonUtf8, &parseError);
			if (parseError.error != QJsonParseError::NoError || !document.isObject())
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("invalid_argument"),
						QStringLiteral("Command arguments must be a JSON object")),
					pCallback, pCallbackContext);
				return;
			}

			const QString strCommandId = QString::fromUtf8(commandIdUtf8);
			const KApplicationCommandResult result = m_pRegistry->execute(
				strCommandId, document.object());
			QJsonObject response{
				{QStringLiteral("status"), static_cast<int>(result.status)},
				{QStringLiteral("value"), result.value},
				{QStringLiteral("errorCode"), result.strErrorCode},
				{QStringLiteral("technicalMessage"), result.strTechnicalMessage}
			};
			completeJson(nRequestId, response, pCallback, pCallbackContext);
			appendEvent(QStringLiteral("command.completed"), QJsonObject{
				{QStringLiteral("requestId"), QString::number(nRequestId)},
				{QStringLiteral("commandId"), strCommandId},
				{QStringLiteral("status"), static_cast<int>(result.status)}
			});
		}, Qt::QueuedConnection);
}

void KAutomationHostBridge::requestSnapshot(quint64 nRequestId,
	const QByteArray &snapshotKindUtf8,
	quint64 nSinceSequence,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	QMetaObject::invokeMethod(this,
		[this, nRequestId, snapshotKindUtf8, nSinceSequence, pCallback, pCallbackContext]()
		{
			if (!m_bAcceptingRequests)
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("application_shutdown"),
						QStringLiteral("Application is shutting down")),
					pCallback, pCallbackContext);
				return;
			}
			const QString strKind = QString::fromUtf8(snapshotKindUtf8);
			if (strKind == QStringLiteral("state"))
				completeJson(nRequestId, stateSnapshot(), pCallback, pCallbackContext);
			else if (strKind == QStringLiteral("events"))
				completeJson(nRequestId, eventsSnapshot(nSinceSequence), pCallback, pCallbackContext);
			else
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("invalid_argument"),
						QStringLiteral("Unknown snapshot kind")),
					pCallback, pCallbackContext);
			}
		}, Qt::QueuedConnection);
}

void KAutomationHostBridge::synchronizeSessionGeneration()
{
	const quint64 nGeneration = m_pSessionController->sessionGeneration();
	const quint64 nPreviousGeneration = m_nObservedSessionGeneration.exchange(nGeneration);
	if (nPreviousGeneration == nGeneration)
		return;

	m_strSignalingState.clear();
	m_strWebRtcState.clear();
	m_strRemoteDeviceId.clear();
	m_strLastError.clear();
	m_negotiatedCapabilities.clear();
	m_privacyModeStatus = KPrivacyModeStatus();
	m_postSessionActionStatus = KPostSessionActionStatus();
	m_nReceivedFrameCount = 0;
	m_nLastFrameTimestampMs = 0;
	m_nLastFrameWidth = 0;
	m_nLastFrameHeight = 0;
	m_bSessionChannelOpen = false;
	m_bInputChannelOpen = false;
	m_bClipboardChannelOpen = false;
	m_bTerminalChannelOpen = false;
}

void KAutomationHostBridge::initializeStateConnections()
{
	m_strSessionState = KSessionStateMachine::stateName(
		m_pSessionController->isIdle() ? IdleSessionState : ConnectedSessionState);
	connect(m_pSessionController, &KSessionController::listeningAvailabilityChanged,
		this, [this](bool bAvailable, quint16 nPort)
		{
			m_bListeningAvailable = bAvailable;
			m_nListeningPort = nPort;
			appendEvent(QStringLiteral("signaling.listening_changed"), QJsonObject{
				{QStringLiteral("available"), bAvailable},
				{QStringLiteral("port"), nPort}
			});
		});
	connect(m_pSessionController, &KSessionController::signalingChanged,
		this, [this](const QString &strState)
		{
			synchronizeSessionGeneration();
			m_strSignalingState = strState;
		});
	connect(m_pSessionController, &KSessionController::webRtcStateChanged,
		this, [this](const QString &strState)
		{
			synchronizeSessionGeneration();
			m_strWebRtcState = strState;
		});
	connect(m_pSessionController, &KSessionController::sessionStateChanged,
		this, [this](KSessionState state)
		{
			synchronizeSessionGeneration();
			m_strSessionState = KSessionStateMachine::stateName(state);
			if (state == ConnectedSessionState || state == StreamingSessionState)
				m_strWebRtcState = QStringLiteral("connected");
			else if (state == ReconnectingSessionState)
				m_strWebRtcState = QStringLiteral("disconnected");
			appendEvent(QStringLiteral("session.state_changed"),
				QJsonObject{{QStringLiteral("state"), m_strSessionState}});
		});
	connect(m_pSessionController, &KSessionController::sessionChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bSessionChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::inputChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bInputChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::clipboardChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bClipboardChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::terminalChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bTerminalChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::sessionCapabilitiesChanged,
		this, [this](const KNegotiatedCapabilities &capabilities)
		{
			synchronizeSessionGeneration();
			m_negotiatedCapabilities = capabilities.channels;
			if (capabilities.bFileTransfer)
				m_negotiatedCapabilities.append(QStringLiteral("fileTransfer"));
		});
	connect(m_pSessionController, &KSessionController::remoteFrameStatsReady,
		this, [this](int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs)
		{
			Q_UNUSED(nFrameIndex);
			Q_UNUSED(nTimestampMs);
			synchronizeSessionGeneration();
			const qint64 nReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
			++m_nReceivedFrameCount;
			m_nLastFrameWidth = nWidth;
			m_nLastFrameHeight = nHeight;
			m_nLastFrameTimestampMs = nReceivedAtMs;
			appendEvent(QStringLiteral("frame.received"), QJsonObject{
				{QStringLiteral("frameCount"), QString::number(m_nReceivedFrameCount)},
				{QStringLiteral("timestampMs"), QString::number(nReceivedAtMs)}
			});
		});
	connect(m_pSessionController, &KSessionController::incomingAccessRequest,
		this, [this](const QString &strRequestId, const QString &strDeviceName,
			const QString &, qint64 nExpiresAtMs)
		{
			appendEvent(QStringLiteral("access.requested"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("deviceName"), strDeviceName},
				{QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs)}
			});
		});
	connect(m_pSessionController, &KSessionController::incomingAccessRequestCleared,
		this, [this](const QString &strRequestId, const QString &strReason)
		{
			appendEvent(QStringLiteral("access.cleared"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("reason"), strReason}
			});
		});
	connect(m_pSessionController, &KSessionController::pairingRequested,
		this, [this](const QString &strRequestId, const QString &strDeviceName,
			const QString &strLocalRole, const QString &strVerificationCode,
			const QString &, const QString &, const QString &, const QString &,
			KPermissionScopes requestedPermissions, qint64 nExpiresAtMs)
		{
			appendEvent(QStringLiteral("pairing.requested"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("deviceName"), strDeviceName},
				{QStringLiteral("localRole"), strLocalRole},
				{QStringLiteral("verificationCode"), strVerificationCode},
				{QStringLiteral("requestedPermissions"),
					QString::number(static_cast<quint32>(requestedPermissions))},
				{QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs)}
			});
		});
	connect(m_pSessionController, &KSessionController::pairingCleared,
		this, [this](const QString &strRequestId, const QString &strReason)
		{
			appendEvent(QStringLiteral("pairing.cleared"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("reason"), strReason}
			});
		});
	connect(m_pSessionController, &KSessionController::deviceAuthenticationStateChanged,
		this, [this](const QString &, const QString &strDeviceId, const QString &, bool)
		{
			synchronizeSessionGeneration();
			m_strRemoteDeviceId = strDeviceId;
		});
	connect(m_pSessionController, &KSessionController::privacyModeStatusChanged,
		this, [this](const KPrivacyModeStatus &status)
		{
			synchronizeSessionGeneration();
			m_privacyModeStatus = status;
		});
	connect(m_pSessionController, &KSessionController::postSessionActionStatusChanged,
		this, [this](const KPostSessionActionStatus &status)
		{
			synchronizeSessionGeneration();
			m_postSessionActionStatus = status;
		});
	connect(m_pSessionController, &KSessionController::privacyModeCommandCompleted,
		this, [this](const QString &, bool bSuccess, const QString &strErrorCode)
		{
			synchronizeSessionGeneration();
			if (!bSuccess)
				m_privacyModeStatus.strErrorCode = strErrorCode;
		});
	connect(m_pSessionController, &KSessionController::postSessionActionCommandCompleted,
		this, [this](const QString &, bool bSuccess, const QString &strErrorCode)
		{
			synchronizeSessionGeneration();
			if (!bSuccess)
				m_postSessionActionStatus.strErrorCode = strErrorCode;
		});
	connect(m_pSessionController, &KSessionController::sessionErrorOccurred,
		this, [this](const KSessionError &error)
		{
			synchronizeSessionGeneration();
			m_strLastError = KSessionError::codeName(error.code);
			appendEvent(QStringLiteral("session.error"), QJsonObject{
				{QStringLiteral("code"), m_strLastError},
				{QStringLiteral("message"), error.strTechnicalMessage}
			});
		});
}

void KAutomationHostBridge::appendEvent(const QString &strType, const QJsonObject &value)
{
	KAutomationEvent event;
	event.nSequence = m_nNextEventSequence++;
	event.strType = strType;
	event.value = value;
	m_events.append(event);
	while (m_events.size() > kMaximumAutomationEvents)
		m_events.removeFirst();
}

QJsonObject KAutomationHostBridge::stateSnapshot() const
{
	QJsonArray capabilities;
	for (const QString &strCapability : m_negotiatedCapabilities)
		capabilities.append(strCapability);
	return QJsonObject{
		{QStringLiteral("role"), KSessionStateMachine::roleName(m_pSessionController->sessionRole())},
		{QStringLiteral("sessionState"), m_strSessionState},
		{QStringLiteral("signalingState"), m_strSignalingState},
		{QStringLiteral("webRtcState"), m_strWebRtcState},
		{QStringLiteral("listeningAvailable"), m_bListeningAvailable},
		{QStringLiteral("listeningPort"), m_nListeningPort},
		{QStringLiteral("sessionChannelOpen"), m_bSessionChannelOpen},
		{QStringLiteral("inputChannelOpen"), m_bInputChannelOpen},
		{QStringLiteral("clipboardChannelOpen"), m_bClipboardChannelOpen},
		{QStringLiteral("terminalChannelOpen"), m_bTerminalChannelOpen},
		{QStringLiteral("remoteDeviceId"), m_strRemoteDeviceId},
		{QStringLiteral("negotiatedCapabilities"), capabilities},
		{QStringLiteral("receivedFrameCount"), QString::number(m_nReceivedFrameCount)},
		{QStringLiteral("lastFrameWidth"), m_nLastFrameWidth},
		{QStringLiteral("lastFrameHeight"), m_nLastFrameHeight},
		{QStringLiteral("lastFrameTimestampMs"), QString::number(m_nLastFrameTimestampMs)},
		{QStringLiteral("privacyModeStatus"), QJsonObject{
			{QStringLiteral("requestedMode"), PrivacyModeName(m_privacyModeStatus.requestedMode)},
			{QStringLiteral("effectiveMode"), PrivacyModeName(m_privacyModeStatus.effectiveMode)},
			{QStringLiteral("state"), PrivacyStateName(m_privacyModeStatus.state)},
			{QStringLiteral("errorCode"), m_privacyModeStatus.strErrorCode}
		}},
		{QStringLiteral("postSessionActionStatus"), QJsonObject{
			{QStringLiteral("action"), PostSessionActionName(m_postSessionActionStatus.action)},
			{QStringLiteral("errorCode"), m_postSessionActionStatus.strErrorCode}
		}},
		{QStringLiteral("lastError"), m_strLastError}
	};
}

QJsonObject KAutomationHostBridge::eventsSnapshot(quint64 nSinceSequence) const
{
	QJsonArray events;
	for (const KAutomationEvent &event : m_events)
	{
		if (event.nSequence <= nSinceSequence)
			continue;
		QJsonObject item = event.value;
		item.insert(QStringLiteral("sequence"), QString::number(event.nSequence));
		item.insert(QStringLiteral("type"), event.strType);
		events.append(item);
	}
	const quint64 nOldest = m_events.isEmpty() ? m_nNextEventSequence : m_events.first().nSequence;
	return QJsonObject{
		{QStringLiteral("events"), events},
		{QStringLiteral("oldestSequence"), QString::number(nOldest)},
		{QStringLiteral("nextSequence"), QString::number(m_nNextEventSequence)},
		{QStringLiteral("hasGap"), nSinceSequence != 0 && nSinceSequence + 1 < nOldest}
	};
}

void KAutomationHostBridge::completeJson(quint64 nRequestId,
	const QJsonObject &object,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext) const
{
	if (pCallback == nullptr)
		return;
	const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
	pCallback(pCallbackContext, nRequestId, json.constData(),
		static_cast<std::uint32_t>(json.size()));
}
