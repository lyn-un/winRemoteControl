#include "automation/driver/base/commandcontext.h"
#include "automation/driver/base/httpserver.h"
#include "automation/driver/base/requestparser.h"
#include "automation/driver/base/requestrouter.h"
#include "automation/driver/base/sessionmanager.h"
#include "automation/driver/base/status.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

#include <atomic>
#include <thread>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	void TestParserAndRouter()
	{
		KParsedDriverRequest request;
		QJsonObject error;
		Check(KRequestParser::parse(QByteArrayLiteral("post"),
			QByteArrayLiteral("/session/abc/state"), QByteArrayLiteral("{}"),
			&request, &error), QStringLiteral("valid request parses"));
		Check(!KRequestParser::parse(QByteArrayLiteral("POST"), QByteArrayLiteral("bad"),
			QByteArrayLiteral("[]"), &request, &error),
			QStringLiteral("invalid path and non-object body are rejected"));
		Check(!KRequestParser::parse(QByteArrayLiteral("POST"), QByteArrayLiteral("/status"),
			QByteArray(KRequestParser::kMaximumBodyBytes + 1, 'x'), &request, &error),
			QStringLiteral("oversized body is rejected"));
		Check(!KRequestParser::parse(QByteArray(), QByteArrayLiteral("/status"),
			QByteArray(), &request, &error), QStringLiteral("empty method is rejected"));
		Check(!KRequestParser::parse(QByteArrayLiteral("GET"),
			QByteArray("/\xFF", 2), QByteArray(), &request, &error),
			QStringLiteral("invalid UTF-8 path is rejected"));
		Check(!KRequestParser::parse(QByteArrayLiteral("POST"), QByteArrayLiteral("/status"),
			QByteArray("{\"value\":\"\xFF\"}", 13), &request, &error),
			QStringLiteral("invalid UTF-8 body is rejected"));

		KRequestRouter router;
		QString strError;
		QString strCapturedSession;
		Check(router.registerRoute(QStringLiteral("GET"),
			QStringLiteral("/session/:sessionId/state"),
			[&strCapturedSession](quint64 nRequestId, const KParsedDriverRequest &,
				const QHash<QString, QString> &parameters)
			{
				if (nRequestId == 88)
					strCapturedSession = parameters.value(QStringLiteral("sessionId"));
			}, &strError), QStringLiteral("route registers"));
		request.strMethod = QStringLiteral("GET");
		request.strPath = QStringLiteral("/session/session-1/state?ignored=true");
		Check(router.route(88, request) && strCapturedSession == QStringLiteral("session-1"),
			QStringLiteral("route extracts path parameter"));
		Check(!router.registerRoute(QStringLiteral("GET"),
			QStringLiteral("/session/:sessionId/state"),
			[](quint64, const auto &, const auto &) {},
			&strError), QStringLiteral("duplicate route is rejected"));
		request.strPath = QStringLiteral("/unknown");
		Check(!router.route(89, request), QStringLiteral("unknown route is rejected"));
	}

	void TestSessionsAndStatus()
	{
		KDriverSessionManager manager;
		const KDriverSession created = manager.createSession(1000, 17, 3);
		Check(created.isValid() && manager.sessionCount() == 1,
			QStringLiteral("driver session is created"));
		Check(manager.session(created.strSessionId, 1200) != nullptr,
			QStringLiteral("driver session can be retrieved"));
		Check(manager.collectExpired(1300, 500) == 0,
			QStringLiteral("active driver session is retained"));
		Check(manager.collectExpired(2000, 500) == 1,
			QStringLiteral("idle driver session is collected"));
		Check(manager.session(QStringLiteral("missing"), 2100) == nullptr,
			QStringLiteral("unknown driver session is rejected"));
		const KDriverSession quitSession = manager.createSession(2200, 19, 4);
		Check(manager.quitSession(quitSession.strSessionId)
			&& manager.session(quitSession.strSessionId, 2201) == nullptr,
			QStringLiteral("quit driver session is removed"));
		const QJsonObject response = DriverErrorResponse(CommandTimeoutDriverStatus,
			QString(), QStringLiteral("timed out"));
		Check(!response.value(QStringLiteral("isSuccess")).toBool()
			&& response.value(QStringLiteral("status")).toInt() == CommandTimeoutDriverStatus,
			QStringLiteral("status maps to stable JSON"));
		const QList<KDriverStatus> statuses{
			OkDriverStatus, InvalidArgumentDriverStatus, InvalidSessionIdDriverStatus,
			UnknownCommandDriverStatus, CommandDisabledDriverStatus, CommandBusyDriverStatus,
			CommandTimeoutDriverStatus, UnsupportedOperationDriverStatus, InternalErrorDriverStatus
		};
		for (const KDriverStatus status : statuses)
		{
			Check(!DriverStatusName(status).isEmpty(),
				QStringLiteral("every driver status has a stable name"));
		}

		KDriverCommandContext context;
		Check(context.complete(DriverSuccessResponse(QJsonObject()))
			&& !context.complete(DriverSuccessResponse(QJsonObject()))
			&& !context.timeout(response),
			QStringLiteral("command context completes exactly once"));
	}

	QByteArray SendHttpRequest(quint16 nPort, const QByteArray &request)
	{
		QTcpSocket socket;
		socket.connectToHost(QHostAddress::LocalHost, nPort);
		if (!socket.waitForConnected(2000))
			return QByteArray();
		if (socket.write(request) != request.size() || !socket.waitForBytesWritten(2000))
			return QByteArray();
		QByteArray response;
		while (socket.waitForReadyRead(2000))
			response.append(socket.readAll());
		response.append(socket.readAll());
		return response;
	}

	QByteArray RunHttpClient(quint16 nPort, const QByteArray &request)
	{
		std::atomic<bool> bDone = false;
		QByteArray response;
		std::thread client([&]()
		{
			response = SendHttpRequest(nPort, request);
			bDone.store(true);
		});
		QElapsedTimer timer;
		timer.start();
		while (!bDone.load() && timer.elapsed() < 5000)
		{
			QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
			QThread::msleep(1);
		}
		client.join();
		return response;
	}

	void TestHttpServer()
	{
		QTemporaryDir temporaryDirectory;
		Check(temporaryDirectory.isValid(), QStringLiteral("HTTP test directory is available"));
		if (!temporaryDirectory.isValid())
			return;

		KHttpServer server;
		QString strError;
		Check(server.start(temporaryDirectory.path(), 4242,
			QStringLiteral("test-build"), &strError),
			QStringLiteral("HTTP server starts on loopback"));
		Check(server.port() != 0 && !server.token().isEmpty(),
			QStringLiteral("HTTP server assigns a port and random token"));
		const QString strDiscoveryPath = temporaryDirectory.filePath(QStringLiteral("4242.json"));
		Check(QFileInfo::exists(strDiscoveryPath),
			QStringLiteral("HTTP server writes its discovery file"));

		const QByteArray unauthorized = RunHttpClient(server.port(),
			QByteArrayLiteral("GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n"));
		Check(unauthorized.startsWith(QByteArrayLiteral("HTTP/1.1 401")),
			QStringLiteral("HTTP server rejects requests without its token"));

		QObject::connect(&server, &KHttpServer::requestReceived, &server,
			[&server](quint64 nRequestId, const QByteArray &method,
				const QByteArray &path, const QByteArray &)
			{
				server.sendJsonResponse(nRequestId, 200, QJsonObject{
					{QStringLiteral("method"), QString::fromLatin1(method)},
					{QStringLiteral("path"), QString::fromLatin1(path)}
				});
			});
		const QByteArray authorizedRequest = QByteArrayLiteral(
			"GET /status HTTP/1.1\r\nHost: localhost\r\nX-WRC-Token: ")
			+ server.token().toUtf8()
			+ QByteArrayLiteral("\r\nContent-Length: 0\r\n\r\n");
		const QByteArray authorized = RunHttpClient(server.port(), authorizedRequest);
		Check(authorized.startsWith(QByteArrayLiteral("HTTP/1.1 200"))
			&& authorized.contains(QByteArrayLiteral("\"path\":\"/status\"")),
			QStringLiteral("HTTP server accepts an authenticated local request"));

		const QByteArray oversizedRequest = QByteArrayLiteral(
			"POST /session HTTP/1.1\r\nHost: localhost\r\nX-WRC-Token: ")
			+ server.token().toUtf8()
			+ QByteArrayLiteral("\r\nContent-Length: 16385\r\n\r\n");
		const QByteArray oversized = RunHttpClient(server.port(), oversizedRequest);
		Check(oversized.startsWith(QByteArrayLiteral("HTTP/1.1 413")),
			QStringLiteral("HTTP server rejects oversized request bodies"));

		server.stop();
		Check(!QFileInfo::exists(strDiscoveryPath),
			QStringLiteral("HTTP server removes its discovery file on shutdown"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestParserAndRouter();
	TestSessionsAndStatus();
	TestHttpServer();
	if (g_nFailureCount == 0)
		qInfo() << "All driver base tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
