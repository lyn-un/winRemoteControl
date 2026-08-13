#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_WINDOWSTERMINALFRONTEND_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_WINDOWSTERMINALFRONTEND_H_

#include "core/terminal/terminalfrontend.h"
#include "core/terminal/terminalrelayframecodec.h"

#include <QtCore/QByteArray>
#include <QtCore/QQueue>

class QLocalServer;
class QLocalSocket;
class QTimer;

class KWindowsTerminalFrontend final : public KTerminalFrontend
{
	Q_OBJECT

public:
	explicit KWindowsTerminalFrontend(QObject *pParent = nullptr);
	~KWindowsTerminalFrontend() override;

	bool isSupported(QString *pReason = nullptr) const override;
	bool open(quint64 nGeneration, const QString &strTitle,
		QString *pErrorMessage = nullptr) override;
	void focus() override;
	bool writeOutput(quint64 nGeneration, const QByteArray &data) override;
	void close(quint64 nGeneration) override;

private:
	void handleNewConnection();
	void handleReadyRead();
	void handleDisconnected();
	void handleBytesWritten();
	bool enqueueOutput(const QByteArray &data);
	void flushPendingOutput();
	bool sendFrame(quint16 nType, const QByteArray &payload = QByteArray());
	bool launchRelay(const QString &strTitle, QString *pErrorMessage);
	void rejectSocket(QLocalSocket *pSocket);
	void clearLocalState(bool bEmitClosed);
	QString relayPath() const;
	QString windowsTerminalPath() const;
	void writeTrace(const QString &strStage, const QString &strExtra = QString()) const;

	QLocalServer *m_pServer = nullptr;
	QLocalSocket *m_pSocket = nullptr;
	QTimer *m_pHandshakeTimer = nullptr;
	KTerminalRelayFrameCodec m_frameCodec;
	QQueue<QByteArray> m_pendingOutput;
	qsizetype m_nPendingOutputBytes = 0;
	quint64 m_nGeneration = 0;
	QString m_strPipeName;
	QString m_strToken;
	QString m_strWindowName;
	bool m_bAuthenticated = false;
	bool m_bClosing = false;
	bool m_bInputObserved = false;
	bool m_bRelayBackpressured = false;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_WINDOWSTERMINALFRONTEND_H_
