#ifndef _WINREMOTECONTROL_DRIVER_WRCMODULE_H_
#define _WINREMOTECONTROL_DRIVER_WRCMODULE_H_

#include "automation/driver/base/commandcontext.h"
#include "automation/driver/base/httpserver.h"
#include "automation/driver/base/requestrouter.h"
#include "automation/driver/base/sessionmanager.h"
#include "automation/wrcdriverhostapi.h"

#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>

class KWrcDriverModule;

struct KWrcDriverCallbackGate
{
	QMutex mutex;
	QPointer<KWrcDriverModule> pModule;
	QSet<quint64> startedRequestIds;
};

class KWrcDriverModule : public QObject
{
	Q_OBJECT

public:
	explicit KWrcDriverModule(QObject *pParent = nullptr);
	~KWrcDriverModule() override;

	bool start(const KWrcDriverHostApiV2 *pHostApi,
		KWrcDriverCallbackGate *pCallbackGate,
		QString *pErrorMessage);
	void stop();

private:
	enum KPendingHostRequestType
	{
		CommandPendingHostRequest,
		SnapshotPendingHostRequest
	};

	struct KPendingHostRequest
	{
		KDriverCommandContext context;
		KPendingHostRequestType type = CommandPendingHostRequest;
		QString strIdempotencyKey;
	};

	static void HostJsonCompleted(void *pCallbackContext,
		std::uint64_t nRequestId,
		const char *pJsonUtf8,
		std::uint32_t nJsonBytes);
	static void HostCommandStarted(void *pCallbackContext,
		std::uint64_t nRequestId);

	void initializeRoutes();
	void handleHttpRequest(quint64 nRequestId,
		const QByteArray &method,
		const QByteArray &path,
		const QByteArray &body);
	void handleStatus(quint64 nRequestId);
	void handleCreateSession(quint64 nRequestId);
	void handleDeleteSession(quint64 nRequestId,
		const QHash<QString, QString> &pathParameters);
	void handleTriggerCommand(quint64 nRequestId,
		const KParsedDriverRequest &request,
		const QHash<QString, QString> &pathParameters);
	void handleStateSnapshot(quint64 nRequestId,
		const QHash<QString, QString> &pathParameters);
	void handleEventsSnapshot(quint64 nRequestId,
		const KParsedDriverRequest &request,
		const QHash<QString, QString> &pathParameters);
	bool validateSession(quint64 nRequestId,
		const QHash<QString, QString> &pathParameters,
		QString *pSessionId);
	void requestHostSnapshot(quint64 nRequestId,
		const QString &strSessionId,
		const QByteArray &kind,
		quint64 nSinceSequence);
	void completeIdempotencyRecord(const KPendingHostRequest &pending,
		const QJsonObject &response);
	void discardIdempotencyRecord(const KPendingHostRequest &pending);
	void handleHostJsonCompleted(quint64 nRequestId, const QByteArray &jsonUtf8);
	bool consumeHostCommandStarted(quint64 nRequestId);
	void beginHostTimeout(quint64 nRequestId, int nTimeoutMs);
	void respond(quint64 nHttpRequestId, const QJsonObject &response, int nStatusCode = 200);
	QString hostValue(const QByteArray &key) const;

	KHttpServer *m_pHttpServer = nullptr;
	KRequestRouter m_router;
	KDriverSessionManager m_sessionManager;
	QHash<quint64, KPendingHostRequest> m_pendingHostRequests;
	const KWrcDriverHostApiV2 *m_pHostApi = nullptr;
	KWrcDriverCallbackGate *m_pCallbackGate = nullptr;
	quint64 m_nNextHostRequestId = 1;
	bool m_bStarted = false;
};

#endif // _WINREMOTECONTROL_DRIVER_WRCMODULE_H_
