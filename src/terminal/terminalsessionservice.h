#ifndef _WINREMOTECONTROL_TERMINAL_TERMINALSESSIONSERVICE_H_
#define _WINREMOTECONTROL_TERMINAL_TERMINALSESSIONSERVICE_H_

#include "core/protocol/sessionmessage.h"
#include "core/protocol/terminalmessage.h"
#include "core/session/sessionstatemachine.h"
#include "core/terminal/terminalstate.h"

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QQueue>

#include <memory>

class KSessionController;
class KTerminalHost;
class QTimer;

class KTerminalSessionService final : public QObject
{
	Q_OBJECT

public:
	explicit KTerminalSessionService(std::unique_ptr<KTerminalHost> spTerminalHost,
		KSessionController *pSessionController,
		QObject *pParent = nullptr);
	~KTerminalSessionService() override;
	bool isHostSupported(QString *pReason = nullptr) const;

	KTerminalSessionService(const KTerminalSessionService &) = delete;
	KTerminalSessionService &operator=(const KTerminalSessionService &) = delete;

public slots:
	void openCurrentTerminal(int nColumns = 100, int nRows = 30);
	void openTerminalForEndpoint(const QString &strHost, quint16 nPort);
	void respondIncomingRequest(const QString &strRequestId, bool bAccepted);
	void sendInput(const QByteArray &data);
	void resizeTerminal(int nColumns, int nRows);
	void closeTerminal();
	void requestState();
	void setApprovalTimeoutSeconds(int nSeconds);
	void shutdown();

signals:
	void stateChanged(KTerminalState state, bool bAvailable, const QString &strStatus,
		const QString &strDeviceName, const QString &strDeviceSource);
	void outputReady(const QByteArray &data);
	void incomingRequest(const QString &strRequestId, const QString &strDeviceName,
		const QString &strDeviceSource, qint64 nExpiresAtMs);
	void incomingRequestCleared(const QString &strRequestId, const QString &strReason);
	void terminalError(const QString &strMessage);
	void focusWindowRequested();

private:
	void handleSessionStateChanged(KSessionState state);
	void handleCapabilitiesChanged(const KNegotiatedCapabilities &capabilities);
	void handleControlMessage(const KTerminalMessage &message);
	void handleTerminalData(const QByteArray &data);
	void handleChannelChanged(bool bOpen);
	void handleHostOutput(quint64 nGeneration, const QByteArray &data);
	void handleHostExited(quint64 nGeneration, int nExitCode);
	void handleApprovalTimeout();
	bool startHost(const QString &strRequestId, int nColumns, int nRows);
	void stopHost(bool bNotifyRemote, const QString &strReason);
	void resetSession();
	void setState(KTerminalState state, const QString &strStatus);
	void enqueueOutput(const QByteArray &data);
	void flushOutput();
	void sendControl(const KTerminalMessage &message);
	bool isSessionReady() const;
	void writeTrace(const QString &strStage, const QString &strExtra = QString()) const;

	std::unique_ptr<KTerminalHost> m_spTerminalHost;
	KSessionController *m_pSessionController = nullptr;
	QTimer *m_pApprovalTimer = nullptr;
	KTerminalState m_state = ClosedTerminalState;
	QString m_strStatus;
	KSessionState m_sessionState = IdleSessionState;
	KNegotiatedCapabilities m_capabilities;
	QQueue<QByteArray> m_outputQueue;
	qsizetype m_nQueuedOutputBytes = 0;
	quint64 m_nInputBytes = 0;
	quint64 m_nOutputBytes = 0;
	QString m_strRequestId;
	QString m_strDeviceName;
	QString m_strDeviceSource;
	QString m_strPendingHost;
	quint16 m_nPendingPort = 0;
	int m_nColumns = 100;
	int m_nRows = 30;
	int m_nApprovalTimeoutSeconds = 30;
	bool m_bChannelOpen = false;
	bool m_bController = false;
	bool m_bPermissionGranted = false;
	bool m_bPermissionDenied = false;
	bool m_bOpenAfterConnect = false;
};

#endif // _WINREMOTECONTROL_TERMINAL_TERMINALSESSIONSERVICE_H_
