#include "automation/driver/app/wrcdrivermodule.h"

#include "automation/driver/base/requestparser.h"
#include "automation/driver/base/status.h"
#include "automation/driver/app/sessioncommands.h"
#include "automation/driver/app/statecommands.h"
#include "automation/driver/app/triggercommand.h"
#include "automation/driver/app/wrcroutes.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>
#include <QtCore/QMutexLocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace
{
	constexpr int kHostCommandTimeoutMs = 5000;
	constexpr int kHostCallbackGraceMs = 1000;
	constexpr qint64 kSessionIdleTimeoutMs = 30 * 60 * 1000;
}

KWrcDriverModule::KWrcDriverModule(QObject *pParent)
	: QObject(pParent)
	, m_pHttpServer(new KHttpServer(this))
{
	m_idempotencyClock.start();
	connect(m_pHttpServer, &KHttpServer::requestReceived,
		this, &KWrcDriverModule::handleHttpRequest);
	initializeRoutes();
}

KWrcDriverModule::~KWrcDriverModule()
{
	stop();
}

bool KWrcDriverModule::start(const KWrcDriverHostApiV2 *pHostApi,
	KWrcDriverCallbackGate *pCallbackGate,
	QString *pErrorMessage)
{
	if (m_bStarted)
		return true;
	if (pHostApi == nullptr || pCallbackGate == nullptr
		|| pHostApi->nAbiVersion != KWrcDriverAbiVersion2
		|| pHostApi->submitCommand == nullptr || pHostApi->requestSnapshot == nullptr
		|| pHostApi->copyHostValue == nullptr || pHostApi->isHostReady == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Invalid Host API");
		return false;
	}
	m_pHostApi = pHostApi;
	m_pCallbackGate = pCallbackGate;
	const QString strDiscoveryDirectory = QDir(
		QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
		.filePath(QStringLiteral("winRemoteControl/automation"));
	const qint64 nPid = hostValue(QByteArrayLiteral("pid")).toLongLong();
	if (nPid <= 0 || !m_pHttpServer->start(strDiscoveryDirectory, nPid,
		hostValue(QByteArrayLiteral("buildId")), pErrorMessage))
	{
		m_pHostApi = nullptr;
		return false;
	}
	m_bStarted = true;
	if (m_pHostApi->writeLog != nullptr)
	{
		const QByteArray message = QByteArrayLiteral("HTTP automation endpoint started on loopback");
		m_pHostApi->writeLog(m_pHostApi->pHostContext, 0,
			message.constData(), static_cast<std::uint32_t>(message.size()));
	}
	return true;
}

void KWrcDriverModule::stop()
{
	if (!m_bStarted)
		return;
	m_bStarted = false;
	const QJsonObject shutdownResponse = DriverErrorResponse(InternalErrorDriverStatus,
		QStringLiteral("application_shutdown"),
		QStringLiteral("Application is shutting down"), false, false);
	for (auto iter = m_pendingHostRequests.begin();
		iter != m_pendingHostRequests.end(); ++iter)
	{
		if (!iter->context.timeout(shutdownResponse))
			continue;
		respond(iter->context.nRequestId, shutdownResponse, 503);
	}
	m_pendingHostRequests.clear();
	m_idempotencyStore.clear();
	if (m_pCallbackGate != nullptr)
	{
		QMutexLocker locker(&m_pCallbackGate->mutex);
		m_pCallbackGate->startedRequestIds.clear();
	}
	m_sessionManager.clear();
	m_pHttpServer->stop();
	m_pHostApi = nullptr;
	m_pCallbackGate = nullptr;
}

void KWrcDriverModule::HostJsonCompleted(void *pCallbackContext,
	std::uint64_t nRequestId,
	const char *pJsonUtf8,
	std::uint32_t nJsonBytes)
{
	auto *pGate = static_cast<KWrcDriverCallbackGate *>(pCallbackContext);
	if (pGate == nullptr)
		return;
	const QByteArray json(pJsonUtf8, static_cast<qsizetype>(nJsonBytes));
	QMutexLocker locker(&pGate->mutex);
	KWrcDriverModule *pModule = pGate->pModule.data();
	if (pModule == nullptr)
		return;
	QMetaObject::invokeMethod(pModule,
		[pModule, nRequestId, json]()
		{
			pModule->handleHostJsonCompleted(nRequestId, json);
		}, Qt::QueuedConnection);
}

void KWrcDriverModule::HostCommandStarted(void *pCallbackContext,
	std::uint64_t nRequestId)
{
	auto *pGate = static_cast<KWrcDriverCallbackGate *>(pCallbackContext);
	if (pGate == nullptr)
		return;
	QMutexLocker locker(&pGate->mutex);
	KWrcDriverModule *pModule = pGate->pModule.data();
	if (pModule == nullptr)
		return;
	pGate->startedRequestIds.insert(nRequestId);
	QMetaObject::invokeMethod(pModule,
		[pModule, nRequestId]()
		{
			pModule->handleHostCommandStarted(nRequestId);
		}, Qt::QueuedConnection);
}

void KWrcDriverModule::initializeRoutes()
{
	KWrcRouteHandlers handlers;
	handlers.status = [this](quint64 nRequestId, const auto &, const auto &)
	{
		handleStatus(nRequestId);
	};
	handlers.createSession = [this](quint64 nRequestId, const auto &, const auto &)
	{
		handleCreateSession(nRequestId);
	};
	handlers.deleteSession = [this](quint64 nRequestId, const auto &,
		const QHash<QString, QString> &pathParameters)
	{
		handleDeleteSession(nRequestId, pathParameters);
	};
	handlers.triggerCommand = [this](quint64 nRequestId,
		const KParsedDriverRequest &request,
		const QHash<QString, QString> &pathParameters)
	{
		handleTriggerCommand(nRequestId, request, pathParameters);
	};
	handlers.stateSnapshot = [this](quint64 nRequestId, const auto &,
		const QHash<QString, QString> &pathParameters)
	{
		handleStateSnapshot(nRequestId, pathParameters);
	};
	handlers.eventsSnapshot = [this](quint64 nRequestId,
		const KParsedDriverRequest &request,
		const QHash<QString, QString> &pathParameters)
	{
		handleEventsSnapshot(nRequestId, request, pathParameters);
	};
	QString strError;
	if (!RegisterWrcRoutes(&m_router, handlers, &strError))
		qFatal("Unable to register automation routes");
}

void KWrcDriverModule::handleHttpRequest(quint64 nRequestId,
	const QByteArray &method,
	const QByteArray &path,
	const QByteArray &body)
{
	KParsedDriverRequest request;
	QJsonObject parseError;
	if (!KRequestParser::parse(method, path, body, &request, &parseError))
	{
		respond(nRequestId, parseError, 400);
		return;
	}
	m_sessionManager.collectExpired(QDateTime::currentMSecsSinceEpoch(), kSessionIdleTimeoutMs);
	if (!m_router.route(nRequestId, request))
	{
		respond(nRequestId, DriverErrorResponse(UnsupportedOperationDriverStatus,
			QStringLiteral("unknown_route"), QStringLiteral("Unknown automation route")), 404);
		return;
	}
}

void KWrcDriverModule::handleStatus(quint64 nRequestId)
{
	const bool bHostReady = m_pHostApi != nullptr
		&& m_pHostApi->isHostReady != nullptr
		&& m_pHostApi->isHostReady(m_pHostApi->pHostContext);
	respond(nRequestId, DriverSuccessResponse(DriverStatusValue(
		hostValue(QByteArrayLiteral("pid")), hostValue(QByteArrayLiteral("buildId")),
		m_bStarted, bHostReady)));
}

void KWrcDriverModule::handleCreateSession(quint64 nRequestId)
{
	if (m_pHostApi == nullptr || m_pHostApi->isHostReady == nullptr
		|| !m_pHostApi->isHostReady(m_pHostApi->pHostContext))
	{
		respond(nRequestId, DriverErrorResponse(InternalErrorDriverStatus,
			QStringLiteral("application_not_ready"),
			QStringLiteral("Application host is not ready"), true, false), 503);
		return;
	}
	bool bCursorOk = false;
	bool bGenerationOk = false;
	const quint64 nEventCursor = hostValue(QByteArrayLiteral("eventCursor"))
		.toULongLong(&bCursorOk);
	const quint64 nSessionGeneration = hostValue(
		QByteArrayLiteral("sessionGeneration")).toULongLong(&bGenerationOk);
	if (!bCursorOk || !bGenerationOk)
	{
		respond(nRequestId, DriverErrorResponse(InternalErrorDriverStatus,
			QStringLiteral("invalid_host_state"),
			QStringLiteral("Host did not provide an event cursor and generation")), 500);
		return;
	}
	const KDriverSession session = m_sessionManager.createSession(
		QDateTime::currentMSecsSinceEpoch(), nEventCursor, nSessionGeneration);
	respond(nRequestId, DriverSuccessResponse(DriverSessionValue(session,
		hostValue(QByteArrayLiteral("pid")), hostValue(QByteArrayLiteral("buildId")))));
}

bool KWrcDriverModule::validateSession(quint64 nRequestId,
	const QHash<QString, QString> &pathParameters,
	QString *pSessionId)
{
	const QString strSessionId = pathParameters.value(QStringLiteral("sessionId"));
	if (strSessionId.isEmpty()
		|| m_sessionManager.session(strSessionId,
			QDateTime::currentMSecsSinceEpoch()) == nullptr)
	{
		respond(nRequestId, DriverErrorResponse(InvalidSessionIdDriverStatus,
			QStringLiteral("invalid_session_id"), QStringLiteral("Unknown driver session")), 404);
		return false;
	}
	if (pSessionId != nullptr)
		*pSessionId = strSessionId;
	return true;
}

void KWrcDriverModule::handleDeleteSession(quint64 nRequestId,
	const QHash<QString, QString> &pathParameters)
{
	QString strSessionId;
	if (!validateSession(nRequestId, pathParameters, &strSessionId))
		return;
	m_sessionManager.quitSession(strSessionId);
	respond(nRequestId, DriverSuccessResponse(QJsonObject()));
}

void KWrcDriverModule::handleTriggerCommand(quint64 nRequestId,
	const KParsedDriverRequest &request,
	const QHash<QString, QString> &pathParameters)
{
	QString strSessionId;
	if (!validateSession(nRequestId, pathParameters, &strSessionId))
		return;
	const QString strCommandId = request.parameters.value(QStringLiteral("id")).toString();
	const QJsonValue argumentsValue = request.parameters.value(QStringLiteral("arguments"));
	const QJsonValue idempotencyValue = request.parameters.value(
		QStringLiteral("idempotencyKey"));
	const QString strIdempotencyKey = idempotencyValue.toString();
	if (strCommandId.isEmpty()
		|| strCommandId.toUtf8().size() > KRequestParser::kMaximumCommandIdBytes
		|| (!idempotencyValue.isUndefined()
			&& (!idempotencyValue.isString() || strIdempotencyKey.isEmpty()))
		|| strIdempotencyKey.toUtf8().size() > KRequestParser::kMaximumCommandIdBytes
		|| (!argumentsValue.isUndefined() && !argumentsValue.isObject()))
	{
		respond(nRequestId, DriverErrorResponse(InvalidArgumentDriverStatus,
			QStringLiteral("invalid_argument"),
			QStringLiteral("Command id and object arguments are required")), 400);
		return;
	}
	const QJsonObject arguments = argumentsValue.isObject()
		? argumentsValue.toObject() : QJsonObject();
	KDriverSession *pSession = m_sessionManager.session(strSessionId,
		QDateTime::currentMSecsSinceEpoch());
	if (pSession == nullptr)
	{
		respond(nRequestId, DriverErrorResponse(InvalidSessionIdDriverStatus,
			QStringLiteral("invalid_session_id"), QStringLiteral("Unknown driver session")), 404);
		return;
	}
	collectExpiredIdempotencyRecords();
	const QByteArray canonicalArguments =
		KDriverIdempotencyStore::canonicalArguments(arguments);
	if (!strIdempotencyKey.isEmpty())
	{
		const KDriverIdempotencyRecord *pExisting =
			m_idempotencyStore.record(strIdempotencyKey);
		if (pExisting != nullptr)
		{
			if (pExisting->state == PendingDriverIdempotencyState
				&& consumeHostCommandStarted(pExisting->nHostRequestId))
			{
				const auto pending = m_pendingHostRequests.find(pExisting->nHostRequestId);
				if (pending != m_pendingHostRequests.end())
					pending->bHostStarted = true;
				m_idempotencyStore.markStarted(
					pExisting->nHostRequestId, idempotencyNowMs());
				pExisting = m_idempotencyStore.record(strIdempotencyKey);
			}
			if (pExisting->strCommandId != strCommandId
				|| pExisting->canonicalArguments != canonicalArguments)
			{
				respond(nRequestId, DriverErrorResponse(InvalidArgumentDriverStatus,
					QStringLiteral("idempotency_key_conflict"),
					QStringLiteral("Idempotency key was used for a different command")), 400);
			}
			else if (pExisting->state == CompletedDriverIdempotencyState)
			{
				respond(nRequestId, pExisting->response, pExisting->nHttpStatusCode);
			}
			else if (pExisting->state == StartedDriverIdempotencyState)
			{
				respond(nRequestId,
					DriverErrorResponse(CommandExecutionStartedDriverStatus,
						QStringLiteral("command_execution_started"),
						QStringLiteral("Command started but has not completed"),
						false, true), 408);
			}
			else
			{
				respond(nRequestId, DriverErrorResponse(CommandBusyDriverStatus,
					QStringLiteral("command_busy"),
					QStringLiteral("Command with this idempotency key is still running"),
					true, false));
			}
			return;
		}
	}
	const quint64 nHostRequestId = m_nNextHostRequestId++;
	if (!strIdempotencyKey.isEmpty()
		&& !m_idempotencyStore.create(strIdempotencyKey,
			strCommandId, canonicalArguments, nHostRequestId, idempotencyNowMs()))
	{
		respond(nRequestId, DriverErrorResponse(CommandBusyDriverStatus,
			QStringLiteral("idempotency_cache_full"),
			QStringLiteral("Too many idempotent commands are active"), true, false));
		return;
	}
	KPendingHostRequest pending;
	pending.context.nRequestId = nRequestId;
	pending.context.strSessionId = strSessionId;
	pending.context.pathParameters = pathParameters;
	pending.context.parameters = request.parameters;
	pending.type = CommandPendingHostRequest;
	pending.strIdempotencyKey = strIdempotencyKey;
	m_pendingHostRequests.insert(nHostRequestId, pending);
	const QByteArray commandIdUtf8 = strCommandId.toUtf8();
	const QByteArray argumentsJson = QJsonDocument(arguments)
		.toJson(QJsonDocument::Compact);
	m_pHostApi->submitCommand(m_pHostApi->pHostContext, nHostRequestId,
		commandIdUtf8.constData(), static_cast<std::uint32_t>(commandIdUtf8.size()),
		argumentsJson.constData(), static_cast<std::uint32_t>(argumentsJson.size()),
		kHostCommandTimeoutMs, &KWrcDriverModule::HostCommandStarted,
		&KWrcDriverModule::HostJsonCompleted, m_pCallbackGate);
	beginHostTimeout(nHostRequestId, kHostCommandTimeoutMs + kHostCallbackGraceMs);
}

void KWrcDriverModule::handleStateSnapshot(quint64 nRequestId,
	const QHash<QString, QString> &pathParameters)
{
	QString strSessionId;
	if (!validateSession(nRequestId, pathParameters, &strSessionId))
		return;
	requestHostSnapshot(nRequestId, strSessionId, DriverStateSnapshotKind(), 0);
}

void KWrcDriverModule::handleEventsSnapshot(quint64 nRequestId,
	const KParsedDriverRequest &request,
	const QHash<QString, QString> &pathParameters)
{
	QString strSessionId;
	if (!validateSession(nRequestId, pathParameters, &strSessionId))
		return;
	quint64 nSinceSequence = 0;
	const QUrlQuery query(QUrl(QStringLiteral("http://localhost") + request.strPath));
	const QString strSince = query.queryItemValue(QStringLiteral("sinceSequence"));
	if (!strSince.isEmpty())
	{
		bool bOk = false;
		nSinceSequence = strSince.toULongLong(&bOk);
		if (!bOk)
		{
			respond(nRequestId, DriverErrorResponse(InvalidArgumentDriverStatus,
				QStringLiteral("invalid_argument"),
				QStringLiteral("sinceSequence must be an unsigned integer")), 400);
			return;
		}
	}
	requestHostSnapshot(nRequestId, strSessionId,
		DriverEventsSnapshotKind(), nSinceSequence);
}

void KWrcDriverModule::requestHostSnapshot(quint64 nRequestId,
	const QString &strSessionId,
	const QByteArray &kind,
	quint64 nSinceSequence)
{
	const quint64 nHostRequestId = m_nNextHostRequestId++;
	KPendingHostRequest pending;
	pending.context.nRequestId = nRequestId;
	pending.context.strSessionId = strSessionId;
	pending.context.pathParameters.insert(QStringLiteral("sessionId"), strSessionId);
	pending.type = SnapshotPendingHostRequest;
	m_pendingHostRequests.insert(nHostRequestId, pending);
	m_pHostApi->requestSnapshot(m_pHostApi->pHostContext, nHostRequestId,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), nSinceSequence,
		&KWrcDriverModule::HostJsonCompleted, m_pCallbackGate);
	beginHostTimeout(nHostRequestId, kHostCommandTimeoutMs);
}

void KWrcDriverModule::handleHostJsonCompleted(quint64 nRequestId,
	const QByteArray &jsonUtf8)
{
	const bool bStartedSignal = consumeHostCommandStarted(nRequestId);
	const auto iter = m_pendingHostRequests.find(nRequestId);
	if (iter == m_pendingHostRequests.end())
		return;
	KPendingHostRequest &pending = iter.value();
	if (bStartedSignal || pending.bHostStarted)
		m_idempotencyStore.markStarted(nRequestId, idempotencyNowMs());
	const QJsonDocument document = QJsonDocument::fromJson(jsonUtf8);
	QJsonObject response;
	int nStatusCode = 200;
	if (!document.isObject())
	{
		response = DriverErrorResponse(InternalErrorDriverStatus,
			QStringLiteral("invalid_host_response"),
			QStringLiteral("Host returned malformed JSON"));
		nStatusCode = 500;
	}
	else if (pending.type == SnapshotPendingHostRequest)
	{
		response = DriverSuccessResponse(document.object());
	}
	else
	{
		response = MapApplicationCommandResponse(document.object());
	}
	if (pending.type == CommandPendingHostRequest
		&& !pending.strIdempotencyKey.isEmpty())
	{
		m_idempotencyStore.complete(nRequestId,
			response, nStatusCode, idempotencyNowMs());
	}
	const bool bShouldRespond = pending.context.complete(response);
	const quint64 nHttpRequestId = pending.context.nRequestId;
	m_pendingHostRequests.erase(iter);
	if (bShouldRespond)
		respond(nHttpRequestId, response, nStatusCode);
}

void KWrcDriverModule::handleHostCommandStarted(quint64 nRequestId)
{
	const bool bStartedSignal = consumeHostCommandStarted(nRequestId);
	const auto iter = m_pendingHostRequests.find(nRequestId);
	if (iter == m_pendingHostRequests.end())
		return;
	if (!bStartedSignal && iter->bHostStarted)
		return;
	iter->bHostStarted = true;
	m_idempotencyStore.markStarted(nRequestId, idempotencyNowMs());
}

bool KWrcDriverModule::consumeHostCommandStarted(quint64 nRequestId)
{
	if (m_pCallbackGate == nullptr)
		return false;
	QMutexLocker locker(&m_pCallbackGate->mutex);
	return m_pCallbackGate->startedRequestIds.remove(nRequestId);
}

void KWrcDriverModule::beginHostTimeout(quint64 nRequestId, int nTimeoutMs)
{
	QTimer::singleShot(nTimeoutMs, this, [this, nRequestId]()
	{
		const auto iter = m_pendingHostRequests.find(nRequestId);
		if (iter == m_pendingHostRequests.end())
			return;
		const bool bExecutionStarted = iter->type == CommandPendingHostRequest
			&& (iter->bHostStarted || consumeHostCommandStarted(nRequestId));
		if (bExecutionStarted)
			m_idempotencyStore.markStarted(nRequestId, idempotencyNowMs());
		const QJsonObject response = bExecutionStarted
			? DriverErrorResponse(CommandExecutionStartedDriverStatus,
				QStringLiteral("command_execution_started"),
				QStringLiteral("Command started but did not complete in the response window"),
				false, true)
			: DriverErrorResponse(CommandTimeoutDriverStatus,
				QStringLiteral("command_timeout"),
				QStringLiteral("Application command timed out"));
		if (!iter->context.timeout(response))
			return;
		const quint64 nHttpRequestId = iter->context.nRequestId;
		const bool bTrackLateCompletion = bExecutionStarted
			&& !iter->strIdempotencyKey.isEmpty();
		if (!bTrackLateCompletion)
		{
			m_idempotencyStore.removeByHostRequestId(nRequestId);
			m_pendingHostRequests.erase(iter);
		}
		respond(nHttpRequestId, response, 408);
	});
}

void KWrcDriverModule::collectExpiredIdempotencyRecords()
{
	const KDriverIdempotencyCleanupResult result =
		m_idempotencyStore.collectExpired(idempotencyNowMs());
	for (const quint64 nHostRequestId : result.vecHostRequestIds)
	{
		const auto pending = m_pendingHostRequests.find(nHostRequestId);
		if (pending == m_pendingHostRequests.end() || !pending->context.bTimedOut)
			continue;
		consumeHostCommandStarted(nHostRequestId);
		m_pendingHostRequests.erase(pending);
	}
	const int nTotalCount = result.nCompletedCount
		+ result.nStartedCount + result.nPendingCount;
	if (nTotalCount == 0 || m_pHostApi == nullptr || m_pHostApi->writeLog == nullptr)
		return;
	const QByteArray message = QStringLiteral(
		"Expired idempotency records removed: completed=%1 started=%2 pending=%3")
		.arg(result.nCompletedCount)
		.arg(result.nStartedCount)
		.arg(result.nPendingCount)
		.toUtf8();
	m_pHostApi->writeLog(m_pHostApi->pHostContext, 1,
		message.constData(), static_cast<std::uint32_t>(message.size()));
}

qint64 KWrcDriverModule::idempotencyNowMs() const
{
	return m_idempotencyClock.elapsed();
}

void KWrcDriverModule::respond(quint64 nHttpRequestId,
	const QJsonObject &response,
	int nStatusCode)
{
	QJsonObject enrichedResponse = response;
	if (!enrichedResponse.value(QStringLiteral("isSuccess")).toBool())
	{
		QJsonObject value = enrichedResponse.value(QStringLiteral("value")).toObject();
		value.insert(QStringLiteral("requestId"), QString::number(nHttpRequestId));
		enrichedResponse.insert(QStringLiteral("value"), value);
	}
	m_pHttpServer->sendJsonResponse(nHttpRequestId, nStatusCode, enrichedResponse);
}

QString KWrcDriverModule::hostValue(const QByteArray &key) const
{
	if (m_pHostApi == nullptr || m_pHostApi->copyHostValue == nullptr)
		return QString();
	const std::uint32_t nRequired = m_pHostApi->copyHostValue(m_pHostApi->pHostContext,
		key.constData(), static_cast<std::uint32_t>(key.size()), nullptr, 0);
	if (nRequired == 0 || nRequired > 32768)
		return QString();
	QByteArray value(static_cast<qsizetype>(nRequired), '\0');
	m_pHostApi->copyHostValue(m_pHostApi->pHostContext,
		key.constData(), static_cast<std::uint32_t>(key.size()),
		value.data(), nRequired);
	return QString::fromUtf8(value.constData());
}
