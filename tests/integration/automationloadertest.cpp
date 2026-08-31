#include "automation/automationpluginloader.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

#include <cstring>
#include <atomic>

namespace
{
	QString g_strDataDirectory;
	std::atomic_bool g_bHostReady{false};
	std::atomic_int g_nIdempotentExecutionCount{0};
	std::atomic_int g_nStartedLateExecutionCount{0};
	std::atomic_bool g_bHoldSnapshots{false};

	void SubmitCommand(void *, std::uint64_t nRequestId,
		const char *pCommandId, std::uint32_t nCommandIdBytes,
		const char *, std::uint32_t, std::uint32_t,
		KWrcDriverCommandStartedCallback pStartedCallback,
		KWrcDriverJsonCallback pCallback, void *pCallbackContext)
	{
		const QByteArray commandId(pCommandId, static_cast<qsizetype>(nCommandIdBytes));
		if (commandId == QByteArrayLiteral("test.execution_started_late")
			&& pStartedCallback != nullptr && pCallback != nullptr)
		{
			++g_nStartedLateExecutionCount;
			pStartedCallback(pCallbackContext, nRequestId);
			QTimer::singleShot(6200, [pCallbackContext, nRequestId, pCallback]()
			{
				const QByteArray response = QByteArrayLiteral(
					"{\"status\":0,\"value\":{\"lateCompleted\":true},"
					"\"errorCode\":\"\",\"technicalMessage\":\"\"}");
				pCallback(pCallbackContext, nRequestId, response.constData(),
					static_cast<std::uint32_t>(response.size()));
			});
		}
		else if (commandId == QByteArrayLiteral("test.execution_started")
			&& pStartedCallback != nullptr)
		{
			pStartedCallback(pCallbackContext, nRequestId);
		}
		else if (commandId == QByteArrayLiteral("test.idempotent")
			&& pCallback != nullptr)
		{
			++g_nIdempotentExecutionCount;
			if (pStartedCallback != nullptr)
				pStartedCallback(pCallbackContext, nRequestId);
			const QByteArray response = QByteArrayLiteral(
				"{\"status\":0,\"value\":{\"executed\":true},\"errorCode\":\"\","
				"\"technicalMessage\":\"\"}");
			pCallback(pCallbackContext, nRequestId, response.constData(),
				static_cast<std::uint32_t>(response.size()));
		}
		else if (commandId == QByteArrayLiteral("test.late_callback")
			&& pCallback != nullptr)
		{
			QTimer::singleShot(6200, [pCallbackContext, nRequestId, pCallback]()
			{
				const QByteArray response = QByteArrayLiteral(
					"{\"status\":0,\"value\":{\"late\":true},\"errorCode\":\"\","
					"\"technicalMessage\":\"\"}");
				pCallback(pCallbackContext, nRequestId, response.constData(),
					static_cast<std::uint32_t>(response.size()));
			});
		}
	}

	void RequestSnapshot(void *, std::uint64_t nRequestId,
		const char *pKind, std::uint32_t nKindBytes,
		std::uint64_t, KWrcDriverJsonCallback pCallback, void *pCallbackContext)
	{
		if (pCallback == nullptr)
			return;
		if (g_bHoldSnapshots.load())
			return;
		const QByteArray kind(pKind, static_cast<qsizetype>(nKindBytes));
		const QByteArray response = kind == QByteArrayLiteral("events")
			? QByteArrayLiteral("{\"events\":[],\"hasGap\":false}")
			: QByteArrayLiteral("{\"sessionState\":\"Idle\"}");
		pCallback(pCallbackContext, nRequestId, response.constData(),
			static_cast<std::uint32_t>(response.size()));
	}

	std::uint32_t CopyHostValue(void *, const char *pKey, std::uint32_t nKeyBytes,
		char *pDestination, std::uint32_t nDestinationBytes)
	{
		const QByteArray key(pKey, static_cast<qsizetype>(nKeyBytes));
		QByteArray value;
		if (key == QByteArrayLiteral("pid"))
			value = QByteArray::number(QCoreApplication::applicationPid());
		else if (key == QByteArrayLiteral("buildId"))
			value = QByteArray(KWrcAutomationBuildId);
		else if (key == QByteArrayLiteral("dataDirectory"))
			value = g_strDataDirectory.toUtf8();
		else if (key == QByteArrayLiteral("eventCursor"))
			value = QByteArrayLiteral("41");
		else if (key == QByteArrayLiteral("sessionGeneration"))
			value = QByteArrayLiteral("7");
		else
			return 0;
		const std::uint32_t nRequired = static_cast<std::uint32_t>(value.size() + 1);
		if (pDestination == nullptr || nDestinationBytes < nRequired)
			return nRequired;
		std::memcpy(pDestination, value.constData(), static_cast<size_t>(value.size()));
		pDestination[value.size()] = '\0';
		return nRequired;
	}

	void WriteLog(void *, std::uint32_t, const char *, std::uint32_t)
	{
	}

	bool IsHostReady(void *)
	{
		return g_bHostReady.load();
	}

	QJsonObject SendRequest(quint16 nPort,
		const QString &strToken,
		const QByteArray &method,
		const QByteArray &path,
		const QByteArray &body = QByteArray(),
		int *pStatusCode = nullptr,
		int nResponseTimeoutMs = 2000)
	{
		QTcpSocket socket;
		socket.connectToHost(QHostAddress::LocalHost, nPort);
		if (!socket.waitForConnected(2000))
			return QJsonObject();
		const QByteArray request = method + ' ' + path
			+ QByteArrayLiteral(" HTTP/1.1\r\nHost: localhost\r\nX-WRC-Token: ")
			+ strToken.toUtf8()
			+ QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ")
			+ QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
		if (socket.write(request) != request.size() || !socket.waitForBytesWritten(2000))
			return QJsonObject();
		QByteArray response;
		while (socket.waitForReadyRead(nResponseTimeoutMs))
			response.append(socket.readAll());
		response.append(socket.readAll());
		const qsizetype nBodyOffset = response.indexOf(QByteArrayLiteral("\r\n\r\n"));
		const QList<QByteArray> statusParts = response.left(response.indexOf('\n')).trimmed()
			.split(' ');
		bool bStatusOk = false;
		const int nStatusCode = statusParts.size() >= 2
			? statusParts.at(1).toInt(&bStatusOk) : 0;
		if (pStatusCode != nullptr)
			*pStatusCode = bStatusOk ? nStatusCode : 0;
		if (!bStatusOk || nBodyOffset < 0)
			return QJsonObject();
		return QJsonDocument::fromJson(response.mid(nBodyOffset + 4)).object();
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	if (application.arguments().size() != 4)
		return 1;
	QTemporaryDir temporaryDirectory;
	if (!temporaryDirectory.isValid())
		return 2;
	g_strDataDirectory = temporaryDirectory.path();
	KWrcDriverHostApiV2 hostApi;
	hostApi.submitCommand = &SubmitCommand;
	hostApi.requestSnapshot = &RequestSnapshot;
	hostApi.copyHostValue = &CopyHostValue;
	hostApi.isHostReady = &IsHostReady;
	hostApi.writeLog = &WriteLog;

	KAutomationPluginLoader missingLoader;
	QString strError;
	if (!missingLoader.load(temporaryDirectory.path(), &hostApi, &strError)
		|| missingLoader.isLoaded() || !strError.isEmpty())
	{
		return 3;
	}

	KAutomationPluginLoader incompleteLoader;
	if (incompleteLoader.load(application.arguments().at(2), &hostApi, &strError)
		|| incompleteLoader.isLoaded()
		|| !strError.contains(QStringLiteral("exports are incomplete")))
	{
		return 4;
	}

	KAutomationPluginLoader badAbiLoader;
	if (badAbiLoader.load(application.arguments().at(3), &hostApi, &strError)
		|| badAbiLoader.isLoaded()
		|| !strError.contains(QStringLiteral("ABI version mismatch")))
	{
		return 5;
	}

	KAutomationPluginLoader loader;
	if (!loader.load(application.arguments().at(1), &hostApi, &strError)
		|| !loader.isLoaded())
	{
		qCritical().noquote() << QStringLiteral("Automation loader failed: %1")
			.arg(strError);
		return 6;
	}
	const QString strDiscoveryPath = QDir(
		QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
		.filePath(QStringLiteral("winRemoteControl/automation/%1.json")
			.arg(QCoreApplication::applicationPid()));
	if (!QFileInfo::exists(strDiscoveryPath))
		return 7;
	QFile discoveryFile(strDiscoveryPath);
	if (!discoveryFile.open(QIODevice::ReadOnly))
		return 8;
	const QJsonObject discovery = QJsonDocument::fromJson(discoveryFile.readAll()).object();
	discoveryFile.close();
	const quint16 nPort = static_cast<quint16>(
		discovery.value(QStringLiteral("port")).toInt());
	const QString strToken = discovery.value(QStringLiteral("token")).toString();
	int nHttpStatus = 0;
	const QJsonObject unavailableStatus = SendRequest(nPort, strToken,
		QByteArrayLiteral("GET"), QByteArrayLiteral("/status"), QByteArray(),
		&nHttpStatus);
	const QJsonObject unavailableValue = unavailableStatus.value(
		QStringLiteral("value")).toObject();
	if (nHttpStatus != 200
		|| !unavailableStatus.value(QStringLiteral("isSuccess")).toBool()
		|| unavailableValue.value(QStringLiteral("ready")).toBool()
		|| !unavailableValue.value(QStringLiteral("driverReady")).toBool()
		|| unavailableValue.value(QStringLiteral("hostReady")).toBool())
	{
		return 9;
	}
	const QJsonObject prematureSession = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), QByteArrayLiteral("/session"),
		QByteArrayLiteral("{}"), &nHttpStatus);
	if (nHttpStatus != 503
		|| prematureSession.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString()
			!= QStringLiteral("application_not_ready"))
	{
		return 10;
	}
	g_bHostReady.store(true);
	const QJsonObject readyStatus = SendRequest(nPort, strToken,
		QByteArrayLiteral("GET"), QByteArrayLiteral("/status"), QByteArray(),
		&nHttpStatus);
	const QJsonObject readyValue = readyStatus.value(QStringLiteral("value")).toObject();
	if (nHttpStatus != 200 || !readyStatus.value(QStringLiteral("isSuccess")).toBool()
		|| !readyValue.value(QStringLiteral("ready")).toBool()
		|| !readyValue.value(QStringLiteral("driverReady")).toBool()
		|| !readyValue.value(QStringLiteral("hostReady")).toBool())
	{
		return 11;
	}
	const QJsonObject wrongMethod = SendRequest(nPort, strToken,
		QByteArrayLiteral("PUT"), QByteArrayLiteral("/status"), QByteArray(),
		&nHttpStatus);
	if (nHttpStatus != 404
		|| wrongMethod.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString() != QStringLiteral("unknown_route"))
	{
		return 12;
	}
	const QJsonObject missingSessionId = SendRequest(nPort, strToken,
		QByteArrayLiteral("GET"), QByteArrayLiteral("/session//state"), QByteArray(),
		&nHttpStatus);
	if (nHttpStatus != 404
		|| missingSessionId.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString() != QStringLiteral("unknown_route"))
	{
		return 13;
	}
	const QJsonObject session = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), QByteArrayLiteral("/session"),
		QByteArrayLiteral("{}"), &nHttpStatus);
	if (nHttpStatus != 200 || !session.value(QStringLiteral("isSuccess")).toBool()
		|| session.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("sessionId")).toString().isEmpty())
	{
		return 14;
	}
	const QString strSessionId = session.value(QStringLiteral("value")).toObject()
		.value(QStringLiteral("sessionId")).toString();
	const QJsonObject sessionValue = session.value(QStringLiteral("value")).toObject();
	if (sessionValue.value(QStringLiteral("eventCursor")).toString()
			!= QStringLiteral("41")
		|| sessionValue.value(QStringLiteral("sessionGeneration")).toString()
			!= QStringLiteral("7"))
	{
		return 12;
	}
	const QByteArray triggerPath = QByteArrayLiteral("/session/")
		+ strSessionId.toUtf8() + QByteArrayLiteral("/command/trigger");
	const QByteArray idempotentBody = QByteArrayLiteral(
		"{\"id\":\"test.idempotent\",\"arguments\":{\"value\":1},"
		"\"idempotencyKey\":\"same-operation\"}");
	const QJsonObject firstIdempotent = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), triggerPath, idempotentBody, &nHttpStatus);
	const QJsonObject secondIdempotent = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), triggerPath, idempotentBody, &nHttpStatus);
	if (!firstIdempotent.value(QStringLiteral("isSuccess")).toBool()
		|| !secondIdempotent.value(QStringLiteral("isSuccess")).toBool()
		|| g_nIdempotentExecutionCount.load() != 1)
	{
		return 13;
	}
	const QJsonObject idempotencyConflict = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), triggerPath,
		QByteArrayLiteral(
			"{\"id\":\"test.idempotent\",\"arguments\":{\"value\":2},"
			"\"idempotencyKey\":\"same-operation\"}"),
		&nHttpStatus);
	if (nHttpStatus != 400
		|| idempotencyConflict.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString()
			!= QStringLiteral("idempotency_key_conflict")
		|| g_nIdempotentExecutionCount.load() != 1)
	{
		return 13;
	}
	const QByteArray statePath = QByteArrayLiteral("/session/")
		+ strSessionId.toUtf8() + QByteArrayLiteral("/state");
	const QByteArray eventsPath = QByteArrayLiteral("/session/")
		+ strSessionId.toUtf8() + QByteArrayLiteral("/events?sinceSequence=41");
	if (!SendRequest(nPort, strToken, QByteArrayLiteral("GET"), statePath)
			.value(QStringLiteral("isSuccess")).toBool()
		|| !SendRequest(nPort, strToken, QByteArrayLiteral("GET"), eventsPath)
			.value(QStringLiteral("isSuccess")).toBool())
	{
		return 14;
	}
	const QJsonObject timeout = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), triggerPath,
		QByteArrayLiteral("{\"id\":\"test.late_callback\",\"arguments\":{}}"),
		&nHttpStatus, 7000);
	if (nHttpStatus != 408 || timeout.value(QStringLiteral("isSuccess")).toBool(true)
		|| timeout.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString() != QStringLiteral("command_timeout"))
	{
		return 15;
	}
	QThread::msleep(1500);
	const QJsonObject statusAfterLateCallback = SendRequest(nPort, strToken,
		QByteArrayLiteral("GET"), QByteArrayLiteral("/status"), QByteArray(),
		&nHttpStatus);
	if (nHttpStatus != 200
		|| !statusAfterLateCallback.value(QStringLiteral("isSuccess")).toBool()
		|| !statusAfterLateCallback.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("ready")).toBool())
	{
		return 16;
	}
	const QJsonObject started = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), triggerPath,
		QByteArrayLiteral("{\"id\":\"test.execution_started_late\","
			"\"arguments\":{},\"idempotencyKey\":\"late-operation\"}"),
		&nHttpStatus, 7000);
	const QJsonObject startedValue = started.value(QStringLiteral("value")).toObject();
	if (nHttpStatus != 408 || started.value(QStringLiteral("isSuccess")).toBool(true)
		|| startedValue.value(QStringLiteral("error")).toString()
			!= QStringLiteral("command_execution_started")
		|| startedValue.value(QStringLiteral("retryable")).toBool(true)
		|| !startedValue.value(QStringLiteral("outcomeUnknown")).toBool())
	{
		return 17;
	}
	const QJsonObject replayedWhileStarted = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), triggerPath,
		QByteArrayLiteral("{\"id\":\"test.execution_started_late\","
			"\"arguments\":{},\"idempotencyKey\":\"late-operation\"}"),
		&nHttpStatus);
	if (nHttpStatus != 408
		|| replayedWhileStarted.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString()
			!= QStringLiteral("command_execution_started")
		|| g_nStartedLateExecutionCount.load() != 1)
	{
		return 17;
	}
	QThread::msleep(400);
	const QByteArray sessionPath = QByteArrayLiteral("/session/") + strSessionId.toUtf8();
	const QJsonObject deleted = SendRequest(nPort, strToken,
		QByteArrayLiteral("DELETE"), sessionPath, QByteArray(), &nHttpStatus);
	if (nHttpStatus != 200 || !deleted.value(QStringLiteral("isSuccess")).toBool())
		return 17;
	const QJsonObject shutdownSession = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), QByteArrayLiteral("/session"),
		QByteArrayLiteral("{}"), &nHttpStatus);
	const QString strShutdownSessionId = shutdownSession.value(QStringLiteral("value"))
		.toObject().value(QStringLiteral("sessionId")).toString();
	if (strShutdownSessionId.isEmpty())
		return 18;
	const QByteArray shutdownTriggerPath = QByteArrayLiteral("/session/")
		+ strShutdownSessionId.toUtf8() + QByteArrayLiteral("/command/trigger");
	const QJsonObject replayedStarted = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), shutdownTriggerPath,
		QByteArrayLiteral("{\"id\":\"test.execution_started_late\","
			"\"arguments\":{},\"idempotencyKey\":\"late-operation\"}"),
		&nHttpStatus);
	if (nHttpStatus != 200 || !replayedStarted.value(QStringLiteral("isSuccess")).toBool()
		|| !replayedStarted.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("lateCompleted")).toBool()
		|| g_nStartedLateExecutionCount.load() != 1)
	{
		return 18;
	}
	QJsonObject shutdownPendingResponse;
	QJsonObject shutdownStateResponse;
	int nShutdownHttpStatus = 0;
	int nShutdownStateHttpStatus = 0;
	g_bHoldSnapshots.store(true);
	std::thread shutdownRequester([&]()
	{
		shutdownPendingResponse = SendRequest(nPort, strToken,
			QByteArrayLiteral("POST"), shutdownTriggerPath,
			QByteArrayLiteral("{\"id\":\"test.no_callback\",\"arguments\":{}}"),
			&nShutdownHttpStatus, 3000);
	});
	const QByteArray shutdownStatePath = QByteArrayLiteral("/session/")
		+ strShutdownSessionId.toUtf8() + QByteArrayLiteral("/state");
	std::thread shutdownStateRequester([&]()
	{
		shutdownStateResponse = SendRequest(nPort, strToken,
			QByteArrayLiteral("GET"), shutdownStatePath, QByteArray(),
			&nShutdownStateHttpStatus, 3000);
	});
	QThread::msleep(100);
	loader.shutdown();
	shutdownRequester.join();
	shutdownStateRequester.join();
	if (nShutdownHttpStatus != 503
		|| shutdownPendingResponse.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString()
			!= QStringLiteral("application_shutdown")
		|| nShutdownStateHttpStatus != 503
		|| shutdownStateResponse.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString()
			!= QStringLiteral("application_shutdown"))
	{
		return 19;
	}
	if (QFileInfo::exists(strDiscoveryPath))
		return 20;
	return 0;
}
