#include "adapters/windows/terminal/windowsterminalfrontend.h"

#include "common/sessiontracelogger.h"
#include "core/terminal/terminalrelayprotocol.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <cstring>

namespace
{
	constexpr qsizetype kMaximumPendingOutputBytes = 1024 * 1024;
	constexpr qint64 kRelayWriteHighWatermarkBytes = 1024 * 1024;
	constexpr qint64 kRelayWriteLowWatermarkBytes = 256 * 1024;
	constexpr int kRelayHandshakeTimeoutMs = 10000;
}

KWindowsTerminalFrontend::KWindowsTerminalFrontend(QObject *pParent)
	: KTerminalFrontend(pParent)
	, m_pServer(new QLocalServer(this))
	, m_pHandshakeTimer(new QTimer(this))
{
	m_pServer->setSocketOptions(QLocalServer::UserAccessOption);
	m_pHandshakeTimer->setSingleShot(true);
	connect(m_pServer, &QLocalServer::newConnection,
		this, &KWindowsTerminalFrontend::handleNewConnection);
	connect(m_pHandshakeTimer, &QTimer::timeout, this, [this]()
		{
			if (m_bAuthenticated)
				return;
			emit terminalError(m_nGeneration, QStringLiteral("relay_handshake_timeout"),
				QStringLiteral("Windows Terminal Relay 连接超时"));
			clearLocalState(true);
		});
}

KWindowsTerminalFrontend::~KWindowsTerminalFrontend()
{
	clearLocalState(false);
}

bool KWindowsTerminalFrontend::isSupported(QString *pReason) const
{
	if (windowsTerminalPath().isEmpty())
	{
		if (pReason != nullptr)
			*pReason = QStringLiteral("未安装 Windows Terminal，请先从 Microsoft Store 安装");
		return false;
	}
	if (!QFileInfo::exists(relayPath()))
	{
		if (pReason != nullptr)
			*pReason = QStringLiteral("缺少 wrcTerminalRelay.exe，请完整复制程序目录");
		return false;
	}
	return true;
}

bool KWindowsTerminalFrontend::open(
	quint64 nGeneration,
	const QString &strTitle,
	QString *pErrorMessage)
{
	if (m_nGeneration == nGeneration && (!m_strWindowName.isEmpty() || m_pSocket != nullptr))
	{
		focus();
		return true;
	}
	QString strReason;
	if (!isSupported(&strReason))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strReason;
		return false;
	}
	clearLocalState(false);
	m_nGeneration = nGeneration;
	m_bClosing = false;
	m_strToken = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
	m_strPipeName = QStringLiteral("wrc-terminal-%1-%2")
		.arg(QCoreApplication::applicationPid())
		.arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
	m_strWindowName = QStringLiteral("wrc-%1-%2")
		.arg(QCoreApplication::applicationPid()).arg(nGeneration);
	QLocalServer::removeServer(m_strPipeName);
	if (!m_pServer->listen(m_strPipeName))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("无法创建本地终端管道：%1").arg(m_pServer->errorString());
		clearLocalState(false);
		return false;
	}
	if (!launchRelay(strTitle, pErrorMessage))
	{
		clearLocalState(false);
		return false;
	}
	m_pHandshakeTimer->start(kRelayHandshakeTimeoutMs);
	writeTrace(QStringLiteral("terminal_relay_launch"));
	return true;
}

void KWindowsTerminalFrontend::focus()
{
	if (m_strWindowName.isEmpty())
		return;
	QProcess::startDetached(windowsTerminalPath(),
		{ QStringLiteral("-w"), m_strWindowName,
			QStringLiteral("focus-tab"), QStringLiteral("-t"), QStringLiteral("0") });
	writeTrace(QStringLiteral("terminal_relay_focus"));
}

bool KWindowsTerminalFrontend::writeOutput(quint64 nGeneration, const QByteArray &data)
{
	if (nGeneration != m_nGeneration || data.isEmpty())
		return false;
	if (!m_bAuthenticated)
		return enqueueOutput(data);
	for (qsizetype nOffset = 0; nOffset < data.size();
		nOffset += KTerminalRelayProtocol::kMaximumPayloadBytes)
	{
		const QByteArray chunk = data.mid(nOffset,
			KTerminalRelayProtocol::kMaximumPayloadBytes);
		if (m_bRelayBackpressured || !m_pendingOutput.isEmpty()
			|| (m_pSocket != nullptr
				&& m_pSocket->bytesToWrite() >= kRelayWriteHighWatermarkBytes))
		{
			m_bRelayBackpressured = true;
			if (!enqueueOutput(chunk))
				return false;
			continue;
		}
		if (!sendFrame(KTerminalRelayProtocol::OutputFrameType, chunk))
		{
			return false;
		}
	}
	return true;
}

void KWindowsTerminalFrontend::close(quint64 nGeneration)
{
	if (nGeneration != 0 && nGeneration != m_nGeneration)
		return;
	m_bClosing = true;
	if (m_bAuthenticated)
		sendFrame(KTerminalRelayProtocol::CloseFrameType);
	clearLocalState(false);
}

void KWindowsTerminalFrontend::handleNewConnection()
{
	while (m_pServer->hasPendingConnections())
	{
		QLocalSocket *pSocket = m_pServer->nextPendingConnection();
		if (m_pSocket != nullptr)
		{
			rejectSocket(pSocket);
			continue;
		}
		m_pSocket = pSocket;
		connect(pSocket, &QLocalSocket::readyRead,
			this, &KWindowsTerminalFrontend::handleReadyRead);
		connect(pSocket, &QLocalSocket::disconnected,
			this, &KWindowsTerminalFrontend::handleDisconnected);
		connect(pSocket, &QLocalSocket::bytesWritten,
			this, [this](qint64) { handleBytesWritten(); });
	}
}

void KWindowsTerminalFrontend::handleReadyRead()
{
	if (m_pSocket == nullptr)
		return;
	QVector<KTerminalRelayFrame> frames;
	QString strError;
	if (!m_frameCodec.append(m_pSocket->readAll(), &frames, &strError))
	{
		emit terminalError(m_nGeneration, QStringLiteral("relay_protocol_error"),
			QStringLiteral("本地终端 Relay 协议无效"));
		clearLocalState(true);
		return;
	}
	for (const KTerminalRelayFrame &frame : frames)
	{
		using namespace KTerminalRelayProtocol;
		if (!m_bAuthenticated)
		{
			if (frame.nType != HelloFrameType
				|| QString::fromUtf8(frame.payload) != m_strToken)
			{
				emit terminalError(m_nGeneration, QStringLiteral("relay_auth_failed"),
					QStringLiteral("本地终端 Relay 身份验证失败"));
				clearLocalState(true);
				return;
			}
			m_bAuthenticated = true;
			m_pHandshakeTimer->stop();
			m_pServer->close();
			writeTrace(QStringLiteral("terminal_relay_connected"));
			emit connected(m_nGeneration);
			flushPendingOutput();
		}
		else if (frame.nType == InputFrameType)
		{
			if (!m_bInputObserved)
			{
				m_bInputObserved = true;
				writeTrace(QStringLiteral("terminal_relay_input"),
					QStringLiteral("bytes=%1").arg(frame.payload.size()));
			}
			emit inputReady(m_nGeneration, frame.payload);
		}
		else if (frame.nType == ResizeFrameType
			&& frame.payload.size() == static_cast<qsizetype>(kResizePayloadBytes))
		{
			const auto *pResize = reinterpret_cast<const std::uint8_t *>(
				frame.payload.constData());
			const quint16 nColumns = ReadUint16(pResize);
			const quint16 nRows = ReadUint16(pResize + 2);
			if (nColumns >= 20 && nColumns <= 400 && nRows >= 5 && nRows <= 200)
				emit resizeRequested(m_nGeneration, nColumns, nRows);
		}
		else if (frame.nType == CloseFrameType)
		{
			const quint64 nGeneration = m_nGeneration;
			clearLocalState(false);
			emit closed(nGeneration);
			return;
		}
		else
		{
			emit terminalError(m_nGeneration, QStringLiteral("relay_protocol_error"),
				QStringLiteral("本地终端 Relay 发送了未知帧"));
			clearLocalState(true);
			return;
		}
	}
}

void KWindowsTerminalFrontend::handleDisconnected()
{
	const bool bNotify = m_bAuthenticated && !m_bClosing;
	writeTrace(QStringLiteral("terminal_relay_closed"));
	clearLocalState(false);
	if (bNotify)
		emit closed(m_nGeneration);
}

void KWindowsTerminalFrontend::handleBytesWritten()
{
	if (m_pSocket == nullptr
		|| m_pSocket->bytesToWrite() > kRelayWriteLowWatermarkBytes)
	{
		return;
	}
	m_bRelayBackpressured = false;
	flushPendingOutput();
}

bool KWindowsTerminalFrontend::enqueueOutput(const QByteArray &data)
{
	if (data.isEmpty())
		return true;
	if (m_pendingOutput.size() >= 256
		|| m_nPendingOutputBytes + data.size() > kMaximumPendingOutputBytes)
	{
		return false;
	}
	m_pendingOutput.enqueue(data);
	m_nPendingOutputBytes += data.size();
	return true;
}

void KWindowsTerminalFrontend::flushPendingOutput()
{
	while (m_bAuthenticated && m_pSocket != nullptr
		&& !m_pendingOutput.isEmpty()
		&& m_pSocket->bytesToWrite() < kRelayWriteHighWatermarkBytes)
	{
		const QByteArray output = m_pendingOutput.head();
		if (!sendFrame(KTerminalRelayProtocol::OutputFrameType, output))
			return;
		m_pendingOutput.dequeue();
		m_nPendingOutputBytes -= output.size();
	}
	if (!m_pendingOutput.isEmpty())
		m_bRelayBackpressured = true;
}

bool KWindowsTerminalFrontend::sendFrame(quint16 nType, const QByteArray &payload)
{
	if (m_pSocket == nullptr || m_pSocket->state() != QLocalSocket::ConnectedState
		|| payload.size() > KTerminalRelayProtocol::kMaximumPayloadBytes)
	{
		return false;
	}
	const QByteArray frame = KTerminalRelayFrameCodec::encode(nType, payload);
	return m_pSocket->write(frame) == frame.size();
}

bool KWindowsTerminalFrontend::launchRelay(
	const QString &strTitle,
	QString *pErrorMessage)
{
	const QStringList arguments = {
		QStringLiteral("-w"), m_strWindowName,
		QStringLiteral("new-tab"), QStringLiteral("--title"), strTitle,
		relayPath(), QStringLiteral("--pipe"), m_strPipeName,
		QStringLiteral("--token"), m_strToken
	};
	if (QProcess::startDetached(windowsTerminalPath(), arguments))
		return true;
	if (pErrorMessage != nullptr)
		*pErrorMessage = QStringLiteral("无法启动 Windows Terminal");
	return false;
}

void KWindowsTerminalFrontend::rejectSocket(QLocalSocket *pSocket)
{
	if (pSocket == nullptr)
		return;
	pSocket->disconnectFromServer();
	pSocket->deleteLater();
}

void KWindowsTerminalFrontend::clearLocalState(bool bEmitClosed)
{
	const quint64 nGeneration = m_nGeneration;
	m_pHandshakeTimer->stop();
	if (m_pSocket != nullptr)
	{
		disconnect(m_pSocket, nullptr, this, nullptr);
		m_pSocket->abort();
		m_pSocket->deleteLater();
		m_pSocket = nullptr;
	}
	m_pServer->close();
	if (!m_strPipeName.isEmpty())
		QLocalServer::removeServer(m_strPipeName);
	m_frameCodec.clear();
	m_pendingOutput.clear();
	m_nPendingOutputBytes = 0;
	m_bAuthenticated = false;
	m_bInputObserved = false;
	m_bRelayBackpressured = false;
	m_strPipeName.clear();
	m_strToken.clear();
	m_strWindowName.clear();
	if (bEmitClosed)
		emit closed(nGeneration);
}

QString KWindowsTerminalFrontend::relayPath() const
{
	return QDir(QCoreApplication::applicationDirPath())
		.filePath(QStringLiteral("wrcTerminalRelay.exe"));
}

QString KWindowsTerminalFrontend::windowsTerminalPath() const
{
	return QStandardPaths::findExecutable(QStringLiteral("wt.exe"));
}

void KWindowsTerminalFrontend::writeTrace(
	const QString &strStage,
	const QString &strExtra) const
{
	KSessionTraceLogger::write(QStringLiteral("controller"), strStage,
		QStringLiteral("terminal_relay"), -1,
		QStringLiteral("generation=%1 %2").arg(m_nGeneration).arg(strExtra));
}
