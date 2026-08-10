#include "terminal/terminalsessionservice.h"

#include "common/sessiontracelogger.h"
#include "core/terminal/terminalfrontend.h"
#include "core/terminal/terminalhost.h"
#include "session/sessioncontroller.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

namespace
{
	constexpr qsizetype kMaximumOutputQueueBytes = 1024 * 1024;
	constexpr qsizetype kOutputChunkBytes = 16 * 1024;
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
	, m_pApprovalTimer(new QTimer(this))
{
	Q_ASSERT(m_spTerminalHost != nullptr);
	Q_ASSERT(m_pSessionController != nullptr);
	m_pApprovalTimer->setSingleShot(true);
	connect(m_pApprovalTimer, &QTimer::timeout,
		this, &KTerminalSessionService::handleApprovalTimeout);
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
		this, &KTerminalSessionService::flushOutput);
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
	connect(m_spTerminalHost.get(), &KTerminalHost::terminalError,
		this, [this](quint64 nGeneration, const QString &strCode, const QString &strTechnical)
		{
			if (nGeneration != m_pSessionController->sessionGeneration())
				return;
			writeTrace(QStringLiteral("terminal_host_error"),
				QStringLiteral("code=%1").arg(strCode));
			emit terminalError(strTechnical);
			setState(FailedTerminalState, QStringLiteral("终端运行失败"));
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
			this, [this](quint64 nGeneration, const QString &, const QString &strTechnical)
			{
				if (nGeneration == m_pSessionController->sessionGeneration())
					emit terminalError(strTechnical);
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
		emit terminalError(strSupportReason);
		setState(FailedTerminalState, QStringLiteral("Windows Terminal 不可用"));
		return;
	}
	if (!m_pSessionController->isIdle())
	{
		if (m_pSessionController->matchesCurrentEndpoint(strHost, nPort))
			openCurrentTerminal(m_nColumns, m_nRows);
		else
			emit terminalError(QStringLiteral("请先断开当前设备，再打开其他设备的终端"));
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
	if (m_pSessionController->isTerminalBackpressured())
	{
		emit terminalError(QStringLiteral("终端输入暂时拥塞"));
		return;
	}
	m_nInputBytes += static_cast<quint64>(data.size());
	for (qsizetype nOffset = 0; nOffset < data.size(); nOffset += 64 * 1024)
	{
		const QByteArray chunk = data.mid(nOffset, 64 * 1024);
		if (!m_pSessionController->sendTerminalData(chunk))
		{
			emit terminalError(QStringLiteral("终端输入发送失败"));
			return;
		}
	}
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
			return;
		}
		m_bController = false;
		m_strRequestId = message.strRequestId;
		m_nColumns = message.nColumns;
		m_nRows = message.nRows;
		if (m_bPermissionGranted)
		{
			startHost(m_strRequestId, m_nColumns, m_nRows);
			return;
		}
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
		return;
	}
	if (message.strRequestId != m_strRequestId)
		return;
	if (message.type == AcceptedTerminalMessageType && m_bController)
	{
		setState(RunningTerminalState, QStringLiteral("PowerShell 已连接"));
		flushPendingControllerOutput();
	}
	else if (message.type == RejectedTerminalMessageType && m_bController)
	{
		setState(FailedTerminalState, QStringLiteral("被控端拒绝了终端请求"));
		emit terminalError(QStringLiteral("被控端拒绝了终端请求"));
		if (m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	}
	else if (message.type == ResizeTerminalMessageType && !m_bController)
		m_spTerminalHost->resize(m_pSessionController->sessionGeneration(),
			message.nColumns, message.nRows);
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
		emit terminalError(QStringLiteral("远程终端发生错误：%1").arg(message.strErrorCode));
		if (m_bController && m_spTerminalFrontend != nullptr)
			m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	}
}

void KTerminalSessionService::handleTerminalData(const QByteArray &data)
{
	if (data.isEmpty())
		return;
	if (m_bController)
	{
		if (m_state == AwaitingApprovalTerminalState
			|| m_state == OpeningTerminalState)
		{
			enqueuePendingControllerOutput(data);
			return;
		}
		if (m_state != RunningTerminalState)
			return;
		m_nOutputBytes += static_cast<quint64>(data.size());
		if (m_spTerminalFrontend == nullptr
			|| !m_spTerminalFrontend->writeOutput(
				m_pSessionController->sessionGeneration(), data))
		{
			emit terminalError(QStringLiteral("无法写入 Windows Terminal"));
		}
		emit outputReady(data);
	}
	else if (m_state == RunningTerminalState && !m_spTerminalHost->writeInput(
		m_pSessionController->sessionGeneration(), data))
		emit terminalError(QStringLiteral("PowerShell 输入队列已满"));
	else if (m_state == RunningTerminalState)
		m_nInputBytes += static_cast<quint64>(data.size());
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
		emit terminalError(QStringLiteral("当前程序未配置 Windows Terminal 前端"));
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
	emit terminalError(strError);
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
	m_nOutputBytes += static_cast<quint64>(data.size());
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
		emit terminalError(strError);
		return false;
	}
	KTerminalMessage accepted;
	accepted.type = AcceptedTerminalMessageType;
	accepted.strRequestId = strRequestId;
	sendControl(accepted);
	setState(RunningTerminalState, QStringLiteral("远程终端正在运行"));
	flushOutput();
	writeTrace(QStringLiteral("terminal_started"));
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
		m_spTerminalHost->requestStop(m_pSessionController->sessionGeneration());
	else
		setState(ClosedTerminalState, QStringLiteral("终端已关闭"));
	if (m_bController && m_spTerminalFrontend != nullptr)
		m_spTerminalFrontend->close(m_pSessionController->sessionGeneration());
	m_outputQueue.clear();
	m_nQueuedOutputBytes = 0;
	m_pendingControllerOutputQueue.clear();
	m_nPendingControllerOutputBytes = 0;
	m_nInputBytes = 0;
	m_nOutputBytes = 0;
}

void KTerminalSessionService::resetSession()
{
	m_pApprovalTimer->stop();
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
	m_pendingControllerOutputQueue.clear();
	m_nPendingControllerOutputBytes = 0;
	setState(ClosedTerminalState, QStringLiteral("终端未连接"));
}

void KTerminalSessionService::setState(KTerminalState state, const QString &strStatus)
{
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
			KTerminalMessage error;
			error.type = ErrorTerminalMessageType;
			error.strRequestId = m_strRequestId;
			error.strErrorCode = QStringLiteral("output_overflow");
			sendControl(error);
			emit terminalError(QStringLiteral("终端输出过快，已关闭终端"));
			stopHost(false, QStringLiteral("output_overflow"));
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
		emit terminalError(QStringLiteral("终端首屏输出过多，已关闭终端"));
		stopHost(true, QStringLiteral("pending_output_overflow"));
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
		if (!m_pSessionController->sendTerminalData(data))
			return;
		m_outputQueue.dequeue();
		m_nQueuedOutputBytes -= data.size();
	}
}

void KTerminalSessionService::sendControl(const KTerminalMessage &message)
{
	if (!m_pSessionController->sendTerminalControlMessage(message))
		emit terminalError(QStringLiteral("终端控制消息发送失败"));
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
