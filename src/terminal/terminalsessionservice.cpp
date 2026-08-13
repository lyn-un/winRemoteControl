#include "terminal/terminalsessionservice.h"

#include "common/sessiontracelogger.h"
#include "core/terminal/terminalfrontend.h"
#include "core/terminal/terminalhost.h"
#include "session/sessioncontroller.h"
#include "terminal/terminalcommanddispatcher.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

namespace
{
	constexpr qsizetype kMaximumOutputQueueBytes = 1024 * 1024;
	constexpr qsizetype kMaximumInputQueueBytes = 256 * 1024;
	constexpr qsizetype kMaximumInputQueueMessages = 256;
	constexpr qsizetype kOutputChunkBytes = 16 * 1024;
	constexpr qsizetype kInputChunkBytes = 16 * 1024;
	constexpr int kHostStopTimeoutMs = 3000;
}

KTerminalSessionService::KTerminalSessionService(
	std::unique_ptr<KTerminalHost> spTerminalHost,
	KSessionController *pSessionController,
	std::unique_ptr<KTerminalFrontend> spTerminalFrontend,
	QObject *pParent)
	: QObject(pParent)
	, m_spTerminalHost(std::move(spTerminalHost))
	, m_spTerminalFrontend(std::move(spTerminalFrontend))
	, m_pSessionController(pSessionController)
	, m_pCommandDispatcher(new KTerminalCommandDispatcher(this))
	, m_pApprovalTimer(new QTimer(this))
	, m_pStopTimer(new QTimer(this))
{
	Q_ASSERT(m_spTerminalHost != nullptr);
	Q_ASSERT(m_pSessionController != nullptr);
	m_pCommandDispatcher->setTransmitFunction([this](const KTerminalMessage &message)
		{ return m_pSessionController->sendTerminalControlMessage(message); });
	m_pCommandDispatcher->setHandler([this](const KTerminalMessage &message,
		QString *pErrorCode)
		{ return executeControlMessage(message, pErrorCode); });
	connect(m_pCommandDispatcher, &KTerminalCommandDispatcher::commandCompleted,
		this, [this](KTerminalMessageType type, const QString &strRequestId,
			const QString &, bool bSuccess, const QString &strErrorCode,
			quint64 nGeneration)
		{
			handleCommandCompleted(type, strRequestId, bSuccess,
				strErrorCode, nGeneration);
		});
	connect(m_pCommandDispatcher, &KTerminalCommandDispatcher::commandTimedOut,
		this, [this](KTerminalMessageType type, const QString &strRequestId,
			const QString &, quint64 nGeneration)
		{
			if (nGeneration != m_pSessionController->sessionGeneration()
				|| strRequestId != m_strRequestId)
				return;
			if (type == CloseTerminalMessageType || type == ExitedTerminalMessageType)
			{
				writeTrace(QStringLiteral("terminal_command_timeout"),
					QStringLiteral("type=%1 remoteStateUnknown=1")
						.arg(KTerminalMessageCodec::typeName(type)));
				return;
			}
			failTerminal(QStringLiteral("command_timeout"),
				QStringLiteral("终端控制命令超时，终端已中止"));
		});
	m_pApprovalTimer->setSingleShot(true);
	m_pStopTimer->setSingleShot(true);
	m_pStopTimer->setInterval(kHostStopTimeoutMs);
	connect(m_pApprovalTimer, &QTimer::timeout,
		this, &KTerminalSessionService::handleApprovalTimeout);
	connect(m_pStopTimer, &QTimer::timeout, this, [this]()
		{
			if (!m_bHostStopPending)
				return;
			reportTerminalError(ShutdownTimeoutSessionErrorCode,
				QStringLiteral("ConPTY shutdown timed out"));
			setState(FailedTerminalState, QStringLiteral("终端关闭超时"));
			writeTrace(QStringLiteral("terminal_stop_timeout"));
		});
	connect(m_pSessionController, &KSessionController::sessionStateChanged,
		this, &KTerminalSessionService::handleSessionStateChanged);
	connect(m_pSessionController, &KSessionController::sessionCapabilitiesChanged,
		this, &KTerminalSessionService::handleCapabilitiesChanged);
	connect(m_pSessionController, &KSessionController::terminalControlMessageReceived,
		this, &KTerminalSessionService::handleControlMessage);
	connect(m_pSessionController, &KSessionController::terminalDataReceived,
		this, &KTerminalSessionService::handleTerminalData);
	connect(m_pSessionController, &KSessionController::terminalChannelChanged,
		this, &KTerminalSessionService::handleChannelChanged);
	connect(m_pSessionController, &KSessionController::terminalLowWatermarkReached,
		this, [this]()
		{
			flushInput();
			flushOutput();
		});
	connect(m_pSessionController, &KSessionController::incomingAccessObserved,
		this, [this](const QString &strDeviceName, const QString &strSourceAddress)
		{
			m_strDeviceName = strDeviceName;
			m_strDeviceSource = strSourceAddress;
		});
	connect(m_pSessionController, &KSessionController::remoteDeviceInfoChanged,
		this, [this](const QString &strComputerName, const QString &, const QString &,
			int, int)
		{
			m_strDeviceName = strComputerName;
			requestState();
		});
	connect(m_spTerminalHost.get(), &KTerminalHost::outputReady,
		this, &KTerminalSessionService::handleHostOutput);
	connect(m_spTerminalHost.get(), &KTerminalHost::processExited,
		this, &KTerminalSessionService::handleHostExited);
	connect(m_spTerminalHost.get(), &KTerminalHost::stopped,
		this, [this](quint64 nGeneration)
		{
			if (nGeneration != m_pSessionController->sessionGeneration()
				|| !m_bHostStopPending)
			{
				return;
			}
			m_bHostStopPending = false;
			m_pStopTimer->stop();
			setState(ClosedTerminalState, QStringLiteral("终端已关闭"));
			writeTrace(QStringLiteral("terminal_stop_finished"));
		});
	connect(m_spTerminalHost.get(), &KTerminalHost::terminalError,
		this, [this](quint64 nGeneration, const QString &strCode, const QString &strTechnical)
		{
			if (nGeneration != m_pSessionController->sessionGeneration())
				return;
			writeTrace(QStringLiteral("terminal_host_error"),
				QStringLiteral("code=%1").arg(strCode));
			reportTerminalError(TerminalUnavailableSessionErrorCode,
				QStringLiteral("%1: %2").arg(strCode, strTechnical));
			failTerminal(strCode, QStringLiteral("终端运行失败"));
		});
	if (m_spTerminalFrontend != nullptr)
	{
		connect(m_spTerminalFrontend.get(), &KTerminalFrontend::connected,
			this, [this](quint64 nGeneration)
			{
				if (nGeneration != m_pSessionController->sessionGeneration())
					return;
				m_bFrontendConnected = true;
				tryOpenPendingTerminal();
			});
		connect(m_spTerminalFrontend.get(), &KTerminalFrontend::inputReady,
			this, [this](quint64 nGeneration, const QByteArray &data)
			{
				if (nGeneration == m_pSessionController->sessionGeneration())
					sendInput(data);
			});
		connect(m_spTerminalFrontend.get(), &KTerminalFrontend::resizeRequested,
			this, [this](quint64 nGeneration, int nColumns, int nRows)
			{
				if (nGeneration == m_pSessionController->sessionGeneration())
					resizeTerminal(nColumns, nRows);
			});
		connect(m_spTerminalFrontend.get(), &KTerminalFrontend::closed,
			this, [this](quint64 nGeneration)
			{
				if (nGeneration != m_pSessionController->sessionGeneration())
					return;
				m_bFrontendConnected = false;
				stopHost(true, QStringLiteral("terminal_relay_closed"));
			});
		connect(m_spTerminalFrontend.get(), &KTerminalFrontend::terminalError,
			this, [this](quint64 nGeneration, const QString &strCode,
				const QString &strTechnical)
			{
				if (nGeneration == m_pSessionController->sessionGeneration())
				{
					reportTerminalError(strCode == QStringLiteral("relay_handshake_timeout")
						|| strCode == QStringLiteral("relay_auth_failed")
						? TerminalRelayHandshakeFailedSessionErrorCode
						: TerminalUnavailableSessionErrorCode,
						QStringLiteral("%1: %2").arg(strCode, strTechnical));
				}
			});
	}
}

KTerminalSessionService::~KTerminalSessionService()
{
	shutdown();
}

bool KTerminalSessionService::isHostSupported(QString *pReason) const
{
	return m_spTerminalHost->isSupported(pReason);
}

bool KTerminalSessionService::isFrontendSupported(QString *pReason) const
{
	if (m_spTerminalFrontend != nullptr)
		return m_spTerminalFrontend->isSupported(pReason);
	if (pReason != nullptr)
		*pReason = QStringLiteral("当前程序未配置 Windows Terminal 前端");
	return false;
}

void KTerminalSessionService::openCurrentTerminal(int nColumns, int nRows)
{
	if (m_state == RunningTerminalState || m_state == AwaitingApprovalTerminalState)
	{
		if (m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->focus();
		return;
	}
	m_bController = true;
	m_nColumns = qBound(20, nColumns, 400);
	m_nRows = qBound(5, nRows, 200);
	if (!ensureFrontendOpen())
		return;
	if (!isSessionReady() || !m_bFrontendConnected)
	{
		m_bOpenAfterConnect = true;
		setState(OpeningTerminalState, QStringLiteral("正在等待终端通道"));
		return;
	}
	m_spTerminalFrontend->focus();
	m_strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	m_nNextSendSequence = 1;
	m_nLastReceivedSequence = 0;
	KTerminalMessage message;
	message.type = OpenRequestTerminalMessageType;
	message.strRequestId = m_strRequestId;
	message.nColumns = m_nColumns;
	message.nRows = m_nRows;
	setState(AwaitingApprovalTerminalState, QStringLiteral("等待被控端允许终端访问"));
	sendControl(message);
	writeTrace(QStringLiteral("terminal_open_requested"));
}

void KTerminalSessionService::openTerminalForEndpoint(const QString &strHost, quint16 nPort)
{
	QString strSupportReason;
	if (!isFrontendSupported(&strSupportReason))
	{
		reportTerminalError(TerminalUnavailableSessionErrorCode, strSupportReason);
		setState(FailedTerminalState, QStringLiteral("Windows Terminal 不可用"));
		return;
	}
	if (!m_pSessionController->isIdle())
	{
		if (m_pSessionController->matchesCurrentEndpoint(strHost, nPort))
			openCurrentTerminal(m_nColumns, m_nRows);
		else
			reportTerminalError(TerminalUnavailableSessionErrorCode,
				QStringLiteral("Another endpoint is already connected"));
		return;
	}
	m_strPendingHost = strHost;
	m_nPendingPort = nPort;
	m_strDeviceSource = strHost;
	m_bOpenAfterConnect = true;
	m_bController = true;
	m_pSessionController->setRole(QStringLiteral("controller"));
	m_pSessionController->connectSignaling(strHost, nPort);
	ensureFrontendOpen();
	setState(OpeningTerminalState, QStringLiteral("正在连接设备"));
}

void KTerminalSessionService::respondIncomingRequest(
	const QString &strRequestId,
	bool bAccepted)
{
	if (m_state != AwaitingApprovalTerminalState || strRequestId != m_strRequestId)
		return;
	m_pApprovalTimer->stop();
	emit incomingRequestCleared(m_strRequestId, bAccepted
		? QStringLiteral("accepted") : QStringLiteral("rejected"));
	if (!bAccepted)
	{
		m_bPermissionDenied = true;
		KTerminalMessage rejection;
		rejection.type = RejectedTerminalMessageType;
		rejection.strRequestId = m_strRequestId;
		rejection.strReason = QStringLiteral("user_rejected");
		sendControl(rejection);
		setState(ClosedTerminalState, QStringLiteral("已拒绝远程终端"));
		writeTrace(QStringLiteral("terminal_rejected"));
		return;
	}
	m_bPermissionGranted = true;
	writeTrace(QStringLiteral("terminal_accepted"));
	startHost(m_strRequestId, m_nColumns, m_nRows);
}

void KTerminalSessionService::sendInput(const QByteArray &data)
{
	if (!m_bController || m_state != RunningTerminalState || data.isEmpty())
		return;
	if (!enqueueInput(data))
		return;
	flushInput();
}

void KTerminalSessionService::resizeTerminal(int nColumns, int nRows)
{
	if (m_state != RunningTerminalState)
		return;
	m_nColumns = qBound(20, nColumns, 400);
	m_nRows = qBound(5, nRows, 200);
	if (m_bController)
	{
		KTerminalMessage message;
		message.type = ResizeTerminalMessageType;
		message.strRequestId = m_strRequestId;
		message.nColumns = m_nColumns;
		message.nRows = m_nRows;
		sendControl(message);
	}
	else
	{
		m_spTerminalHost->resize(m_pSessionController->sessionGeneration(),
			m_nColumns, m_nRows);
	}
}

void KTerminalSessionService::closeTerminal()
{
	stopHost(true, QStringLiteral("local_close"));
}

void KTerminalSessionService::requestState()
{
	setState(m_state, m_strStatus.isEmpty() ? TerminalStateName(m_state) : m_strStatus);
}

void KTerminalSessionService::setApprovalTimeoutSeconds(int nSeconds)
{
	m_nApprovalTimeoutSeconds = qBound(10, nSeconds, 120);
}

void KTerminalSessionService::shutdown()
{
	m_bOpenAfterConnect = false;
	stopHost(false, QStringLiteral("application_shutdown"));
	if (m_spTerminalFrontend != nullptr)
		m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
}

void KTerminalSessionService::handleSessionStateChanged(KSessionState state)
{
	m_sessionState = state;
	tryOpenPendingTerminal();
	if (state == ReconnectingSessionState && m_state == RunningTerminalState)
	{
		if (m_bController && m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->writeOutput(m_pSessionController->sessionGeneration(),
				QByteArray("\r\n[winRemoteControl] Network reconnecting; input paused.\r\n"));
		setState(PausedTerminalState, QStringLiteral("网络恢复中，终端输入已暂停"));
		return;
	}
	if ((state == ConnectedSessionState || state == StreamingSessionState)
		&& m_state == PausedTerminalState)
	{
		setState(RunningTerminalState, QStringLiteral("终端已恢复"));
		if (m_bController && m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->writeOutput(m_pSessionController->sessionGeneration(),
				QByteArray("[winRemoteControl] Network restored.\r\n"));
		flushOutput();
		return;
	}
	if (state == IdleSessionState || state == ListeningSessionState
		|| state == StoppingSessionState || state == ShutdownTimedOutSessionState)
	{
		stopHost(false, QStringLiteral("session_ended"));
		resetSession();
	}
}

void KTerminalSessionService::handleCapabilitiesChanged(
	const KNegotiatedCapabilities &capabilities)
{
	m_capabilities = capabilities;
	tryOpenPendingTerminal();
	requestState();
}

void KTerminalSessionService::handleControlMessage(const KTerminalMessage &message)
{
	m_pCommandDispatcher->handleIncoming(message,
		m_pSessionController->sessionGeneration());
}

bool KTerminalSessionService::executeControlMessage(
	const KTerminalMessage &message,
	QString *pErrorCode)
{
	if (message.type == OpenRequestTerminalMessageType)
	{
		const bool bBusy = m_state != ClosedTerminalState
			&& m_state != FailedTerminalState;
		if (!isSessionReady() || m_bPermissionDenied || bBusy)
		{
			KTerminalMessage rejection;
			rejection.type = RejectedTerminalMessageType;
			rejection.strRequestId = message.strRequestId;
			rejection.strReason = bBusy
				? QStringLiteral("busy") : QStringLiteral("unavailable");
			sendControl(rejection);
			if (pErrorCode != nullptr)
				*pErrorCode = rejection.strReason;
			return false;
		}
		m_bController = false;
		m_strRequestId = message.strRequestId;
		m_nNextSendSequence = 1;
		m_nLastReceivedSequence = 0;
		m_nColumns = message.nColumns;
		m_nRows = message.nRows;
		if (m_bPermissionGranted)
			return startHost(m_strRequestId, m_nColumns, m_nRows);
		KTerminalMessage pending;
		pending.type = ApprovalPendingTerminalMessageType;
		pending.strRequestId = m_strRequestId;
		pending.nTimeoutSeconds = m_nApprovalTimeoutSeconds;
		sendControl(pending);
		m_pApprovalTimer->start(m_nApprovalTimeoutSeconds * 1000);
		setState(AwaitingApprovalTerminalState, QStringLiteral("等待本机确认远程终端"));
		emit incomingRequest(m_strRequestId, m_strDeviceName, m_strDeviceSource,
			QDateTime::currentMSecsSinceEpoch() + m_nApprovalTimeoutSeconds * 1000LL);
		writeTrace(QStringLiteral("terminal_approval_pending"));
		return true;
	}
	if (message.strRequestId != m_strRequestId)
	{
		if (pErrorCode != nullptr)
			*pErrorCode = QStringLiteral("request_mismatch");
		return false;
	}
	if (message.type == ApprovalPendingTerminalMessageType && m_bController
		&& m_state == AwaitingApprovalTerminalState)
	{
		setState(AwaitingApprovalTerminalState, QStringLiteral("等待被控端确认终端访问"));
	}
	else if (message.type == AcceptedTerminalMessageType && m_bController
		&& (m_state == AwaitingApprovalTerminalState
			|| m_state == OpeningTerminalState))
	{
		setState(OpeningTerminalState, QStringLiteral("正在建立终端数据通道"));
		setState(RunningTerminalState, QStringLiteral("PowerShell 已连接"));
		flushPendingControllerOutput();
	}
	else if (message.type == RejectedTerminalMessageType && m_bController
		&& (m_state == AwaitingApprovalTerminalState
			|| m_state == OpeningTerminalState))
	{
		setState(FailedTerminalState, QStringLiteral("被控端拒绝了终端请求"));
		reportTerminalError(TerminalApprovalRejectedSessionErrorCode,
			message.strReason);
		if (m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	}
	else if (message.type == ResizeTerminalMessageType && !m_bController)
	{
		if (m_state != RunningTerminalState && m_state != PausedTerminalState)
		{
			if (pErrorCode != nullptr)
				*pErrorCode = QStringLiteral("invalid_state");
			return false;
		}
		if (!m_spTerminalHost->resize(m_pSessionController->sessionGeneration(),
			message.nColumns, message.nRows))
		{
			if (pErrorCode != nullptr)
				*pErrorCode = QStringLiteral("resize_failed");
			return false;
		}
	}
	else if (message.type == CloseTerminalMessageType)
		stopHost(false, message.strReason);
	else if (message.type == ExitedTerminalMessageType && m_bController)
	{
		setState(ClosedTerminalState,
			QStringLiteral("PowerShell 已退出（%1）").arg(message.nExitCode));
		if (m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	}
	else if (message.type == ErrorTerminalMessageType)
	{
		setState(FailedTerminalState, QStringLiteral("远程终端发生错误"));
		reportTerminalError(message.strErrorCode == QStringLiteral("input_overflow")
			? TerminalInputOverflowSessionErrorCode
			: (message.strErrorCode == QStringLiteral("output_overflow")
				? TerminalOutputOverflowSessionErrorCode
				: TerminalUnavailableSessionErrorCode),
			message.strErrorCode);
		if (m_bController && m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	}
	else
	{
		if (pErrorCode != nullptr)
			*pErrorCode = QStringLiteral("invalid_state");
		return false;
	}
	return true;
}

void KTerminalSessionService::handleCommandCompleted(
	KTerminalMessageType type,
	const QString &strRequestId,
	bool bSuccess,
	const QString &strErrorCode,
	quint64 nGeneration)
{
	if (nGeneration != m_pSessionController->sessionGeneration()
		|| strRequestId != m_strRequestId)
	{
		return;
	}
	if (type == AcceptedTerminalMessageType && !m_bController)
	{
		if (bSuccess)
		{
			setState(RunningTerminalState, QStringLiteral("远程终端正在运行"));
			flushOutput();
			writeTrace(QStringLiteral("terminal_started"));
		}
		else
		{
			failTerminal(strErrorCode.isEmpty() ? QStringLiteral("accepted_failed")
				: strErrorCode, QStringLiteral("终端启动确认失败，已安全关闭"));
		}
	}
}

void KTerminalSessionService::handleTerminalData(const QByteArray &data)
{
	if (data.isEmpty())
		return;
	KTerminalDataFrame frame;
	QString strDecodeError;
	if (!KTerminalDataFrameCodec::decode(data, &frame, &strDecodeError))
	{
		writeTrace(QStringLiteral("terminal_data_dropped"),
			QStringLiteral("reason=malformed"));
		return;
	}
	if (frame.strRequestId != m_strRequestId)
	{
		writeTrace(QStringLiteral("terminal_data_dropped"),
			QStringLiteral("reason=request_mismatch sequence=%1")
				.arg(frame.nSequence));
		return;
	}
	if (frame.nSequence <= m_nLastReceivedSequence)
	{
		writeTrace(QStringLiteral("terminal_data_dropped"),
			QStringLiteral("reason=sequence sequence=%1 lastSequence=%2")
				.arg(frame.nSequence)
				.arg(m_nLastReceivedSequence));
		return;
	}
	const KTerminalDataDirection expectedDirection = m_bController
		? OutputTerminalDataDirection : InputTerminalDataDirection;
	if (frame.direction != expectedDirection)
	{
		writeTrace(QStringLiteral("terminal_data_dropped"),
			QStringLiteral("reason=direction sequence=%1").arg(frame.nSequence));
		return;
	}
	m_nLastReceivedSequence = frame.nSequence;
	const QByteArray &payload = frame.payload;
	if (m_bController)
	{
		if (m_state == AwaitingApprovalTerminalState
			|| m_state == OpeningTerminalState)
		{
			enqueuePendingControllerOutput(payload);
			return;
		}
		if (m_state != RunningTerminalState)
			return;
		m_nOutputBytes += static_cast<quint64>(payload.size());
		if (m_spTerminalFrontend == nullptr
			|| !m_spTerminalFrontend->writeOutput(
				m_pSessionController->sessionGeneration(), payload))
		{
			reportTerminalError(TerminalUnavailableSessionErrorCode,
				QStringLiteral("Unable to write terminal relay output"));
		}
		emit outputReady(payload);
	}
	else if (m_state == RunningTerminalState && !m_spTerminalHost->writeInput(
		m_pSessionController->sessionGeneration(), payload))
	{
		failTerminal(QStringLiteral("input_overflow"),
			QStringLiteral("PowerShell 输入队列已满，终端已中止"));
	}
	else if (m_state == RunningTerminalState)
		m_nInputBytes += static_cast<quint64>(payload.size());
}

void KTerminalSessionService::handleChannelChanged(bool bOpen)
{
	const bool bWasOpen = m_bChannelOpen;
	m_bChannelOpen = bOpen;
	if (!bOpen && bWasOpen && m_state != ClosedTerminalState)
		stopHost(false, QStringLiteral("terminal_channel_closed"));
	if (bOpen)
		tryOpenPendingTerminal();
	requestState();
}

void KTerminalSessionService::tryOpenPendingTerminal()
{
	if (!m_bOpenAfterConnect || !isSessionReady()
		|| (m_bController && !m_bFrontendConnected))
		return;
	m_bOpenAfterConnect = false;
	openCurrentTerminal(m_nColumns, m_nRows);
}

bool KTerminalSessionService::ensureFrontendOpen()
{
	if (m_spTerminalFrontend == nullptr)
	{
		reportTerminalError(TerminalUnavailableSessionErrorCode,
			QStringLiteral("Windows Terminal frontend is not configured"));
		return false;
	}
	QString strError;
	const QString strTitle = QStringLiteral("winRemoteControl - %1")
		.arg(m_strDeviceName.isEmpty() ? m_strDeviceSource : m_strDeviceName);
	if (m_spTerminalFrontend->open(m_pSessionController->sessionGeneration(),
		strTitle, &strError))
	{
		return true;
	}
	reportTerminalError(TerminalUnavailableSessionErrorCode, strError);
	setState(FailedTerminalState, QStringLiteral("Windows Terminal 不可用"));
	return false;
}

void KTerminalSessionService::handleHostOutput(
	quint64 nGeneration,
	const QByteArray &data)
{
	if (nGeneration != m_pSessionController->sessionGeneration()
		|| m_bController
		|| (m_state != OpeningTerminalState
			&& m_state != RunningTerminalState
			&& m_state != PausedTerminalState))
	{
		return;
	}
		enqueueOutput(data);
}

void KTerminalSessionService::handleHostExited(quint64 nGeneration, int nExitCode)
{
	if (nGeneration != m_pSessionController->sessionGeneration())
		return;
	KTerminalMessage message;
	message.type = ExitedTerminalMessageType;
	message.strRequestId = m_strRequestId;
	message.nExitCode = nExitCode;
	sendControl(message);
	setState(ClosedTerminalState, QStringLiteral("PowerShell 已退出"));
	writeTrace(QStringLiteral("terminal_exited"), QStringLiteral("exitCode=%1").arg(nExitCode));
}

void KTerminalSessionService::handleApprovalTimeout()
{
	if (m_state != AwaitingApprovalTerminalState || m_bController)
		return;
	m_bPermissionDenied = true;
	emit incomingRequestCleared(m_strRequestId, QStringLiteral("timeout"));
	KTerminalMessage message;
	message.type = RejectedTerminalMessageType;
	message.strRequestId = m_strRequestId;
	message.strReason = QStringLiteral("timeout");
	sendControl(message);
	setState(ClosedTerminalState, QStringLiteral("终端授权已超时"));
	writeTrace(QStringLiteral("terminal_approval_timeout"));
}

bool KTerminalSessionService::startHost(
	const QString &strRequestId,
	int nColumns,
	int nRows)
{
	setState(OpeningTerminalState, QStringLiteral("正在启动 PowerShell"));
	QString strError;
	if (!m_spTerminalHost->start(m_pSessionController->sessionGeneration(),
		nColumns, nRows, &strError))
	{
		KTerminalMessage error;
		error.type = ErrorTerminalMessageType;
		error.strRequestId = strRequestId;
		error.strErrorCode = QStringLiteral("host_start_failed");
		sendControl(error);
		setState(FailedTerminalState, QStringLiteral("PowerShell 启动失败"));
		reportTerminalError(TerminalHostStartFailedSessionErrorCode, strError);
		return false;
	}
	KTerminalMessage accepted;
	accepted.type = AcceptedTerminalMessageType;
	accepted.strRequestId = strRequestId;
	if (!sendControl(accepted))
	{
		m_spTerminalHost->requestStop(m_pSessionController->sessionGeneration());
		setState(FailedTerminalState, QStringLiteral("终端启动确认发送失败"));
		return false;
	}
	setState(OpeningTerminalState, QStringLiteral("等待终端启动确认"));
	return true;
}

void KTerminalSessionService::stopHost(bool bNotifyRemote, const QString &strReason)
{
	if (m_state == ClosedTerminalState)
		return;
	writeTrace(QStringLiteral("terminal_close"),
		QStringLiteral("reason=%1 inputBytes=%2 outputBytes=%3")
			.arg(strReason)
			.arg(m_nInputBytes)
			.arg(m_nOutputBytes));
	m_pApprovalTimer->stop();
	if (bNotifyRemote && !m_strRequestId.isEmpty() && m_bChannelOpen)
	{
		KTerminalMessage message;
		message.type = CloseTerminalMessageType;
		message.strRequestId = m_strRequestId;
		message.strReason = strReason;
		sendControl(message);
	}
	setState(ClosingTerminalState, QStringLiteral("正在关闭终端"));
	if (!m_bController)
	{
		m_bHostStopPending = true;
		m_pStopTimer->start();
		m_spTerminalHost->requestStop(m_pSessionController->sessionGeneration());
	}
	else
		setState(ClosedTerminalState, QStringLiteral("终端已关闭"));
	if (m_bController && m_spTerminalFrontend != nullptr)
		m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	m_outputQueue.clear();
	m_nQueuedOutputBytes = 0;
	m_inputQueue.clear();
	m_nQueuedInputBytes = 0;
	m_pendingControllerOutputQueue.clear();
	m_nPendingControllerOutputBytes = 0;
	m_nInputBytes = 0;
	m_nOutputBytes = 0;
	m_nNextSendSequence = 1;
	m_nLastReceivedSequence = 0;
}

void KTerminalSessionService::resetSession()
{
	m_pApprovalTimer->stop();
	m_pStopTimer->stop();
	m_bHostStopPending = false;
	if (m_spTerminalFrontend != nullptr)
		m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	m_capabilities = KNegotiatedCapabilities();
	m_bChannelOpen = false;
	m_bFrontendConnected = false;
	m_bPermissionGranted = false;
	m_bPermissionDenied = false;
	m_bOpenAfterConnect = false;
	m_strRequestId.clear();
	m_strDeviceName.clear();
	m_strDeviceSource.clear();
	m_strPendingHost.clear();
	m_nPendingPort = 0;
	m_outputQueue.clear();
	m_nQueuedOutputBytes = 0;
	m_inputQueue.clear();
	m_nQueuedInputBytes = 0;
	m_pendingControllerOutputQueue.clear();
	m_nPendingControllerOutputBytes = 0;
	m_nNextSendSequence = 1;
	m_nLastReceivedSequence = 0;
	m_pCommandDispatcher->clear();
	setState(ClosedTerminalState, QStringLiteral("终端未连接"));
}

void KTerminalSessionService::setState(KTerminalState state, const QString &strStatus)
{
	if (!m_stateMachine.transitionTo(state))
	{
		writeTrace(QStringLiteral("terminal_state_rejected"),
			QStringLiteral("from=%1 to=%2")
				.arg(TerminalStateName(m_stateMachine.state()), TerminalStateName(state)));
		return;
	}
	m_state = state;
	m_strStatus = strStatus;
	QString strReason;
	const bool bLocalSupported = m_bController
		? isFrontendSupported(&strReason)
		: m_spTerminalHost->isSupported(&strReason);
	const bool bAvailable = bLocalSupported && m_bChannelOpen
		&& m_capabilities.channels.contains(QStringLiteral("terminal"));
	emit stateChanged(state, bAvailable, strStatus, m_strDeviceName, m_strDeviceSource);
}

void KTerminalSessionService::enqueueOutput(const QByteArray &data)
{
	for (qsizetype nOffset = 0; nOffset < data.size(); nOffset += kOutputChunkBytes)
	{
		const QByteArray chunk = data.mid(nOffset, kOutputChunkBytes);
		if (m_nQueuedOutputBytes + chunk.size() > kMaximumOutputQueueBytes)
		{
			failTerminal(QStringLiteral("output_overflow"),
				QStringLiteral("终端输出过快，已关闭终端"));
			return;
		}
		m_outputQueue.enqueue(chunk);
		m_nQueuedOutputBytes += chunk.size();
	}
	flushOutput();
}

void KTerminalSessionService::enqueuePendingControllerOutput(const QByteArray &data)
{
	if (m_nPendingControllerOutputBytes + data.size() > kMaximumOutputQueueBytes)
	{
		failTerminal(QStringLiteral("pending_output_overflow"),
			QStringLiteral("终端首屏输出过多，已关闭终端"));
		return;
	}
	m_pendingControllerOutputQueue.enqueue(data);
	m_nPendingControllerOutputBytes += data.size();
}

void KTerminalSessionService::flushPendingControllerOutput()
{
	while (!m_pendingControllerOutputQueue.isEmpty())
	{
		const QByteArray data = m_pendingControllerOutputQueue.dequeue();
		m_nPendingControllerOutputBytes -= data.size();
		m_nOutputBytes += static_cast<quint64>(data.size());
		if (m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->writeOutput(m_pSessionController->sessionGeneration(), data);
		emit outputReady(data);
	}
}

void KTerminalSessionService::flushOutput()
{
	while (!m_outputQueue.isEmpty()
		&& m_state == RunningTerminalState
		&& !m_pSessionController->isTerminalBackpressured())
	{
		const QByteArray data = m_outputQueue.head();
		if (!sendDataFrame(OutputTerminalDataDirection, data, &m_nNextSendSequence))
			return;
		m_outputQueue.dequeue();
		m_nQueuedOutputBytes -= data.size();
	}
}

bool KTerminalSessionService::enqueueInput(const QByteArray &data)
{
	for (qsizetype nOffset = 0; nOffset < data.size(); nOffset += kInputChunkBytes)
	{
		const QByteArray chunk = data.mid(nOffset, kInputChunkBytes);
		if (m_inputQueue.size() >= kMaximumInputQueueMessages
			|| m_nQueuedInputBytes + chunk.size() > kMaximumInputQueueBytes)
		{
			failTerminal(QStringLiteral("input_overflow"),
				QStringLiteral("终端输入队列已满，终端已中止"));
			return false;
		}
		m_inputQueue.enqueue(chunk);
		m_nQueuedInputBytes += chunk.size();
	}
	return true;
}

void KTerminalSessionService::flushInput()
{
	while (!m_inputQueue.isEmpty()
		&& m_state == RunningTerminalState
		&& !m_pSessionController->isTerminalBackpressured())
	{
		const QByteArray data = m_inputQueue.head();
		if (!sendDataFrame(InputTerminalDataDirection, data, &m_nNextSendSequence))
			return;
		m_inputQueue.dequeue();
		m_nQueuedInputBytes -= data.size();
		m_nInputBytes += static_cast<quint64>(data.size());
	}
}

bool KTerminalSessionService::sendDataFrame(
	KTerminalDataDirection direction,
	const QByteArray &payload,
	quint64 *pSequence)
{
	if (pSequence == nullptr || m_strRequestId.isEmpty())
		return false;
	KTerminalDataFrame frame;
	frame.direction = direction;
	frame.strRequestId = m_strRequestId;
	frame.nSequence = *pSequence;
	frame.payload = payload;
	const QByteArray encoded = KTerminalDataFrameCodec::encode(frame);
	if (encoded.isEmpty() || !m_pSessionController->sendTerminalData(encoded))
		return false;
	++(*pSequence);
	return true;
}

void KTerminalSessionService::failTerminal(
	const QString &strErrorCode,
	const QString &strMessage)
{
	if (m_state == ClosingTerminalState || m_state == ClosedTerminalState
		|| m_state == FailedTerminalState)
	{
		return;
	}
	KTerminalMessage error;
	error.type = ErrorTerminalMessageType;
	error.strRequestId = m_strRequestId;
	error.strErrorCode = strErrorCode;
	sendControl(error);
	KSessionErrorCode code = TerminalUnavailableSessionErrorCode;
	if (strErrorCode == QStringLiteral("input_overflow"))
		code = TerminalInputOverflowSessionErrorCode;
	else if (strErrorCode == QStringLiteral("output_overflow")
		|| strErrorCode == QStringLiteral("pending_output_overflow"))
		code = TerminalOutputOverflowSessionErrorCode;
	else if (strErrorCode == QStringLiteral("command_timeout"))
		code = TerminalCommandTimeoutSessionErrorCode;
	reportTerminalError(code, strErrorCode);
	setState(FailedTerminalState, strMessage);
	stopHost(true, strErrorCode);
}

void KTerminalSessionService::reportTerminalError(
	KSessionErrorCode code,
	const QString &strTechnicalMessage,
	bool bRetryable)
{
	KSessionError error;
	error.domain = TerminalSessionErrorDomain;
	error.code = code;
	error.stage = ConnectedSessionErrorStage;
	error.bRetryable = bRetryable;
	error.strTechnicalMessage = strTechnicalMessage;
	emit structuredTerminalError(error);
}

bool KTerminalSessionService::sendControl(const KTerminalMessage &message)
{
	if (!m_pCommandDispatcher->send(message,
		m_pSessionController->sessionGeneration()))
	{
		reportTerminalError(TerminalUnavailableSessionErrorCode,
			QStringLiteral("Unable to send terminal control command"));
		return false;
	}
	return true;
}

bool KTerminalSessionService::isSessionReady() const
{
	return (m_sessionState == ConnectedSessionState || m_sessionState == StreamingSessionState)
		&& m_bChannelOpen
		&& m_capabilities.bValid
		&& m_capabilities.channels.contains(QStringLiteral("terminal"));
}

void KTerminalSessionService::writeTrace(
	const QString &strStage,
	const QString &strExtra) const
{
	KSessionTraceLogger::write(m_bController
		? QStringLiteral("controller") : QStringLiteral("controlled"),
		strStage, QStringLiteral("terminal"), -1,
		QStringLiteral("generation=%1 requestId=%2 %3")
			.arg(m_pSessionController->sessionGeneration())
			.arg(m_strRequestId, strExtra));
}
