#ifndef _WINREMOTECONTROL_TERMINAL_TERMINALCOMMANDDISPATCHER_H_
#define _WINREMOTECONTROL_TERMINAL_TERMINALCOMMANDDISPATCHER_H_

#include "core/protocol/terminalmessage.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QQueue>

#include <functional>

class KTerminalCommandDispatcher final : public QObject
{
	Q_OBJECT

public:
	using TransmitFunction = std::function<bool(const KTerminalMessage &)>;
	using Handler = std::function<bool(const KTerminalMessage &, QString *)>;

	explicit KTerminalCommandDispatcher(QObject *pParent = nullptr);
	void setTransmitFunction(TransmitFunction transmitFunction);
	void setHandler(Handler handler);
	bool send(KTerminalMessage message, quint64 nGeneration);
	void handleIncoming(const KTerminalMessage &message, quint64 nGeneration);
	void clear();

signals:
	void commandCompleted(KTerminalMessageType type, const QString &strRequestId,
		const QString &strCommandId, bool bSuccess, const QString &strErrorCode,
		quint64 nGeneration);
	void commandTimedOut(KTerminalMessageType type, const QString &strRequestId,
		const QString &strCommandId, quint64 nGeneration);

private:
	struct KPendingCommand
	{
		KTerminalMessage message;
		qint64 nSentMs = 0;
		int nAttempts = 0;
		quint64 nGeneration = 0;
		qsizetype nEncodedBytes = 0;
	};

	void handleTimer();
	void rememberResult(const KTerminalMessage &result);
	bool hasPendingResize() const;
	void sendDeferredResize();

	QHash<QString, KPendingCommand> m_pending;
	QHash<QString, KTerminalMessage> m_recentResults;
	QQueue<QString> m_recentResultIds;
	KTerminalMessage m_deferredResize;
	quint64 m_nDeferredResizeGeneration = 0;
	qsizetype m_nPendingBytes = 0;
	qsizetype m_nRecentResultBytes = 0;
	QElapsedTimer m_clock;
	class QTimer *m_pTimer = nullptr;
	TransmitFunction m_transmitFunction;
	Handler m_handler;
};

#endif // _WINREMOTECONTROL_TERMINAL_TERMINALCOMMANDDISPATCHER_H_
