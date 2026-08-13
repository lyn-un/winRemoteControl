#ifndef _WINREMOTECONTROL_TERMINAL_TERMINALSESSIONSERVICE_H_
#define _WINREMOTECONTROL_TERMINAL_TERMINALSESSIONSERVICE_H_

#include "core/protocol/sessionmessage.h"
#include "core/protocol/terminaldataframe.h"
#include "core/protocol/terminalmessage.h"
#include "core/session/sessionerror.h"
#include "core/session/sessionstatemachine.h"
#include "core/terminal/terminalstate.h"
#include "core/terminal/terminalstatemachine.h"

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QQueue>

#include <memory>

class KSessionController;
class KTerminalFrontend;
class KTerminalHost;
class KTerminalCommandDispatcher;
class QTimer;

class KTerminalSessionService final : public QObject
{
	Q_OBJECT

public:
	explicit KTerminalSessionService(std::unique_ptr<KTerminalHost> spTerminalHost,
		KSessionController *pSessionController,
		std::unique_ptr<KTerminalFrontend> spTerminalFrontend = nullptr,
		QObject *pParent = nullptr);
	~KTerminalSessionService() override;
	bool isHostSupported(QString *pReason = nullptr) const;
	bool isFrontendSupported(QString *pReason = nullptr) const;

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
	void structuredTerminalError(const KSessionError &error);

private:
	void handleSessionStateChanged(KSessionState state);
	void handleCapabilitiesChanged(const KNegotiatedCapabilities &capabilities);
	void handleControlMessage(const KTerminalMessage &message);
	bool executeControlMessage(const KTerminalMessage &message,
		QString *pErrorCode);
	void handleCommandCompleted(KTerminalMessageType type,
		const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode,
		quint64 nGeneration);
	void handleTerminalData(const QByteArray &data);
	void handleChannelChanged(bool bOpen);
	void handleHostOutput(quint64 nGeneration, const QByteArray &data);
	void handleHostExited(quint64 nGeneration, int nExitCode);
	void handleApprovalTimeout();
	bool ensureFrontendOpen();
	void tryOpenPendingTerminal();
	bool startHost(const QString &strRequestId, int nColumns, int nRows);
	void stopHost(bool bNotifyRemote, const QString &strReason);
	void resetSession();
	void setState(KTerminalState state, const QString &strStatus);
	void enqueueOutput(const QByteArray &data);
	void flushOutput();
	bool enqueueInput(const QByteArray &data);
	void flushInput();
	bool sendDataFrame(KTerminalDataDirection direction,
		const QByteArray &payload,
		quint64 *pSequence);
	void reportTerminalError(KSessionErrorCode code,
		const QString &strTechnicalMessage,
		bool bRetryable = false);
	void failTerminal(const QString &strErrorCode, const QString &strMessage);
	void enqueuePendingControllerOutput(const QByteArray &data);
	void flushPendingControllerOutput();
	bool sendControl(const KTerminalMessage &message);
	bool isSessionReady() const;
	void writeTrace(const QString &strStage, const QString &strExtra = QString()) const;

	std::unique_ptr<KTerminalHost> m_spTerminalHost;
	std::unique_ptr<KTerminalFrontend> m_spTerminalFrontend;
	KSessionController *m_pSessionController = nullptr;
	KTerminalCommandDispatcher *m_pCommandDispatcher = nullptr;
	QTimer *m_pApprovalTimer = nullptr;
	QTimer *m_pStopTimer = nullptr;
	KTerminalState m_state = ClosedTerminalState;
	KTerminalStateMachine m_stateMachine;
	QString m_strStatus;
	KSessionState m_sessionState = IdleSessionState;
	KNegotiatedCapabilities m_capabilities;
	QQueue<QByteArray> m_outputQueue;
	qsizetype m_nQueuedOutputBytes = 0;
	QQueue<QByteArray> m_inputQueue;
	qsizetype m_nQueuedInputBytes = 0;
	QQueue<QByteArray> m_pendingControllerOutputQueue;
	qsizetype m_nPendingControllerOutputBytes = 0;
	quint64 m_nInputBytes = 0;
	quint64 m_nOutputBytes = 0;
	quint64 m_nNextSendSequence = 1;
	quint64 m_nLastReceivedSequence = 0;
	QString m_strRequestId;
	QString m_strDeviceName;
	QString m_strDeviceSource;
	QString m_strPendingHost;
	quint16 m_nPendingPort = 0;
	int m_nColumns = 100;
	int m_nRows = 30;
	int m_nApprovalTimeoutSeconds = 30;
	bool m_bChannelOpen = false;
	bool m_bFrontendConnected = false;
	bool m_bController = false;
	bool m_bPermissionGranted = false;
	bool m_bPermissionDenied = false;
	bool m_bOpenAfterConnect = false;
	bool m_bHostStopPending = false;
};

#endif // _WINREMOTECONTROL_TERMINAL_TERMINALSESSIONSERVICE_H_
