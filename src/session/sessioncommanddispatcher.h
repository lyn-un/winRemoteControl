#ifndef _WINREMOTECONTROL_SESSION_SESSIONCOMMANDDISPATCHER_H_
#define _WINREMOTECONTROL_SESSION_SESSIONCOMMANDDISPATCHER_H_

#include "core/protocol/protocolrouter.h"
#include "core/protocol/sessionmessage.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QQueue>

#include <functional>

struct KSessionIncomingDispatchResult
{
	KProtocolHandlerResult handlerResult;
	bool bEndSessionAccepted = false;
};

struct KSessionCommandTransmitResult
{
	bool bAccepted = false;
	QString strErrorCode;
};

class KSessionCommandDispatcher : public QObject
{
	Q_OBJECT

public:
	using Handler = std::function<KProtocolHandlerResult(const KSessionMessage &)>;
	using TransmitFunction = std::function<KSessionCommandTransmitResult(
		const KSessionMessage &)>;

	explicit KSessionCommandDispatcher(QObject *pParent = nullptr);

	KSessionCommandDispatcher(const KSessionCommandDispatcher &) = delete;
	KSessionCommandDispatcher &operator=(const KSessionCommandDispatcher &) = delete;

	void setTransmitFunction(TransmitFunction transmitFunction);
	void registerHandler(KSessionMessageType type, Handler handler);
	QString send(KSessionMessage message, quint64 nGeneration);
	KSessionIncomingDispatchResult handleIncoming(const KSessionMessage &message,
		quint64 nGeneration);
	void failAll(const QString &strErrorCode);
	void clear();

signals:
	void commandCompleted(KSessionMessageType type,
		const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode,
		quint64 nGeneration);
	void commandTimedOut(KSessionMessageType type,
		const QString &strRequestId,
		quint64 nGeneration);

private:
	struct KPendingCommand
	{
		KSessionMessage message;
		qint64 nSentMs = 0;
		int nAttempts = 0;
		quint64 nGeneration = 0;
	};

	KSessionCommandTransmitResult transmit(const KSessionMessage &message) const;
	void handleTimer();
	void handleCommandResult(const KSessionMessage &message, quint64 nGeneration);
	KSessionMessage createCommandResult(const QString &strRequestId,
		const KProtocolHandlerResult &handlerResult) const;
	void rememberCommandResult(const KSessionMessage &message);

	QHash<int, Handler> m_handlers;
	QHash<QString, KPendingCommand> m_pendingCommands;
	QHash<QString, KSessionMessage> m_recentCommandResults;
	QQueue<QString> m_recentCommandResultIds;
	QElapsedTimer m_clock;
	class QTimer *m_pTimer = nullptr;
	TransmitFunction m_transmitFunction;
};

#endif // _WINREMOTECONTROL_SESSION_SESSIONCOMMANDDISPATCHER_H_
