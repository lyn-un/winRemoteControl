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
	constexpr qint64 kSessionIdleTimeoutMs = 30 * 60 * 1000;
}

KWrcDriverModule::KWrcDriverModule(QObject *pParent)
	: QObject(pParent)
	, m_pHttpServer(new KHttpServer(this))
{
	connect(m_pHttpServer, &KHttpServer::requestReceived,
		this, &KWrcDriverModule::handleHttpRequest);
	initializeRoutes();
}

KWrcDriverModule::~KWrcDriverModule()
{
	stop();
}

bool KWrcDriverModule::start(const KWrcDriverHostApiV1 *pHostApi,
	KWrcDriverCallbackGate *pCallbackGate,
	QString *pErrorMessage)
{
	if (m_bStarted)
		return true;
	if (pHostApi == nullptr || pCallbackGate == nullptr
		|| pHostApi->nAbiVersion != KWrcDriverAbiVersion1
		|| pHostApi->submitCommand == nullptr || pHostApi->requestSnapshot == nullptr
		|| pHostApi->copyHostValue == nullptr)
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
	m_pHttpServer->stop();
	m_pendingHostRequests.clear();
	m_sessionManager.clear();
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

void KWrcDriverModule::initializeRoutes()
{
	QString strError;
	if (!RegisterWrcRoutes(&m_router, &strError))
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
	if (!m_router.route(request))
	{
		respond(nRequestId, DriverErrorResponse(UnsupportedOperationDriverStatus,
			QStringLiteral("unknown_route"), QStringLiteral("Unknown automation route")), 404);
		return;
	}
	m_sessionManager.collectExpired(QDateTime::currentMSecsSinceEpoch(), kSessionIdleTimeoutMs);
	const QString strPath = request.strPath.section(QLatin1Char('?'), 0, 0);
	const QStringList segments = strPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);

	if (request.strMethod == QStringLiteral("GET") && strPath == QStringLiteral("/status"))
	{
		respond(nRequestId, DriverSuccessResponse(DriverStatusValue(
			hostValue(QByteArrayLiteral("pid")),
			hostValue(QByteArrayLiteral("buildId")), m_bStarted)));
		return;
	}
	if (request.strMethod == QStringLiteral("POST") && strPath == QStringLiteral("/session"))
	{
		const KDriverSession session = m_sessionManager.createSession(
			QDateTime::currentMSecsSinceEpoch());
		respond(nRequestId, DriverSuccessResponse(DriverSessionValue(session,
			hostValue(QByteArrayLiteral("pid")),
			hostValue(QByteArrayLiteral("buildId")))));
		return;
	}
	if (segments.size() < 2 || segments.first() != QStringLiteral("session"))
	{
		respond(nRequestId, DriverErrorResponse(UnsupportedOperationDriverStatus,
			QStringLiteral("unknown_route"), QStringLiteral("Unknown automation route")), 404);
		return;
	}
	const QString strSessionId = segments.at(1);
	if (m_sessionManager.session(strSessionId, QDateTime::currentMSecsSinceEpoch()) == nullptr)
	{
		respond(nRequestId, DriverErrorResponse(InvalidSessionIdDriverStatus,
			QStringLiteral("invalid_session_id"), QStringLiteral("Unknown driver session")), 404);
		return;
	}
	if (request.strMethod == QStringLiteral("DELETE") && segments.size() == 2)
	{
		m_sessionManager.quitSession(strSessionId);
		respond(nRequestId, DriverSuccessResponse(QJsonObject()));
		return;
	}
	if (request.strMethod == QStringLiteral("POST") && segments.size() == 4
		&& segments.at(2) == QStringLiteral("command")
		&& segments.at(3) == QStringLiteral("trigger"))
	{
		const QString strCommandId = request.parameters.value(QStringLiteral("id")).toString();
		const QJsonValue argumentsValue = request.parameters.value(QStringLiteral("arguments"));
		if (strCommandId.isEmpty()
			|| strCommandId.toUtf8().size() > KRequestParser::kMaximumCommandIdBytes
			|| (!argumentsValue.isUndefined() && !argumentsValue.isObject()))
		{
			respond(nRequestId, DriverErrorResponse(InvalidArgumentDriverStatus,
				QStringLiteral("invalid_argument"),
				QStringLiteral("Command id and object arguments are required")), 400);
			return;
		}
		const quint64 nHostRequestId = m_nNextHostRequestId++;
		KPendingHostRequest pending;
		pending.context.nRequestId = nRequestId;
		pending.context.strSessionId = strSessionId;
		pending.context.pathParameters.insert(QStringLiteral("sessionId"), strSessionId);
		pending.context.parameters = request.parameters;
		pending.type = CommandPendingHostRequest;
		m_pendingHostRequests.insert(nHostRequestId, pending);
		const QByteArray commandIdUtf8 = strCommandId.toUtf8();
		const QByteArray argumentsJson = QJsonDocument(
			argumentsValue.isObject() ? argumentsValue.toObject() : QJsonObject())
			.toJson(QJsonDocument::Compact);
		m_pHostApi->submitCommand(m_pHostApi->pHostContext, nHostRequestId,
			commandIdUtf8.constData(), static_cast<std::uint32_t>(commandIdUtf8.size()),
			argumentsJson.constData(), static_cast<std::uint32_t>(argumentsJson.size()),
			&KWrcDriverModule::HostJsonCompleted, m_pCallbackGate);
		beginHostTimeout(nHostRequestId);
		return;
	}
	if (request.strMethod == QStringLiteral("GET") && segments.size() == 3
		&& (segments.at(2) == QStringLiteral("state")
			|| segments.at(2) == QStringLiteral("events")))
	{
		const QByteArray kind = segments.at(2) == QStringLiteral("state")
			? DriverStateSnapshotKind() : DriverEventsSnapshotKind();
		quint64 nSinceSequence = 0;
		if (kind == QByteArrayLiteral("events"))
		{
			const QUrlQuery query(QUrl(QStringLiteral("http://localhost") + request.strPath));
			bool bOk = false;
			const QString strSince = query.queryItemValue(QStringLiteral("sinceSequence"));
			if (!strSince.isEmpty())
			{
				nSinceSequence = strSince.toULongLong(&bOk);
				if (!bOk)
				{
					respond(nRequestId, DriverErrorResponse(InvalidArgumentDriverStatus,
						QStringLiteral("invalid_argument"),
						QStringLiteral("sinceSequence must be an unsigned integer")), 400);
					return;
				}
			}
		}
		const quint64 nHostRequestId = m_nNextHostRequestId++;
		KPendingHostRequest pending;
		pending.context.nRequestId = nRequestId;
		pending.context.strSessionId = strSessionId;
		pending.context.pathParameters.insert(QStringLiteral("sessionId"), strSessionId);
		pending.context.parameters = request.parameters;
		pending.type = SnapshotPendingHostRequest;
		m_pendingHostRequests.insert(nHostRequestId, pending);
		m_pHostApi->requestSnapshot(m_pHostApi->pHostContext, nHostRequestId,
			kind.constData(), static_cast<std::uint32_t>(kind.size()), nSinceSequence,
			&KWrcDriverModule::HostJsonCompleted, m_pCallbackGate);
		beginHostTimeout(nHostRequestId);
		return;
	}
	respond(nRequestId, DriverErrorResponse(UnsupportedOperationDriverStatus,
		QStringLiteral("unknown_route"), QStringLiteral("Unknown automation route")), 404);
}

void KWrcDriverModule::handleHostJsonCompleted(quint64 nRequestId,
	const QByteArray &jsonUtf8)
{
	const auto iter = m_pendingHostRequests.find(nRequestId);
	if (iter == m_pendingHostRequests.end())
		return;
	KPendingHostRequest &pending = iter.value();
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
	if (!pending.context.complete(response))
		return;
	const quint64 nHttpRequestId = pending.context.nRequestId;
	m_pendingHostRequests.erase(iter);
	respond(nHttpRequestId, response, nStatusCode);
}

void KWrcDriverModule::beginHostTimeout(quint64 nRequestId)
{
	QTimer::singleShot(kHostCommandTimeoutMs, this, [this, nRequestId]()
	{
		const auto iter = m_pendingHostRequests.find(nRequestId);
		if (iter == m_pendingHostRequests.end())
			return;
		const QJsonObject response = DriverErrorResponse(CommandTimeoutDriverStatus,
			QStringLiteral("command_timeout"),
			QStringLiteral("Application command timed out"));
		if (!iter->context.timeout(response))
			return;
		const quint64 nHttpRequestId = iter->context.nRequestId;
		m_pendingHostRequests.erase(iter);
		respond(nHttpRequestId, response, 408);
	});
}

void KWrcDriverModule::respond(quint64 nHttpRequestId,
	const QJsonObject &response,
	int nStatusCode)
{
	m_pHttpServer->sendJsonResponse(nHttpRequestId, nStatusCode, response);
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
