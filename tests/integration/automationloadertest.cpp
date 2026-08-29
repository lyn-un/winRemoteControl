#include "automation/automationpluginloader.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

#include <cstring>

namespace
{
	QString g_strDataDirectory;

	void SubmitCommand(void *, std::uint64_t, const char *, std::uint32_t,
		const char *, std::uint32_t, KWrcDriverJsonCallback, void *)
	{
	}

	void RequestSnapshot(void *, std::uint64_t, const char *, std::uint32_t,
		std::uint64_t, KWrcDriverJsonCallback, void *)
	{
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
	KWrcDriverHostApiV1 hostApi;
	hostApi.submitCommand = &SubmitCommand;
	hostApi.requestSnapshot = &RequestSnapshot;
	hostApi.copyHostValue = &CopyHostValue;
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
	const QJsonObject status = SendRequest(nPort, strToken,
		QByteArrayLiteral("GET"), QByteArrayLiteral("/status"), QByteArray(),
		&nHttpStatus);
	if (nHttpStatus != 200 || !status.value(QStringLiteral("isSuccess")).toBool()
		|| !status.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("ready")).toBool())
	{
		return 9;
	}
	const QJsonObject session = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), QByteArrayLiteral("/session"),
		QByteArrayLiteral("{}"), &nHttpStatus);
	if (nHttpStatus != 200 || !session.value(QStringLiteral("isSuccess")).toBool()
		|| session.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("sessionId")).toString().isEmpty())
	{
		return 10;
	}
	const QString strSessionId = session.value(QStringLiteral("value")).toObject()
		.value(QStringLiteral("sessionId")).toString();
	const QByteArray triggerPath = QByteArrayLiteral("/session/")
		+ strSessionId.toUtf8() + QByteArrayLiteral("/command/trigger");
	const QJsonObject timeout = SendRequest(nPort, strToken,
		QByteArrayLiteral("POST"), triggerPath,
		QByteArrayLiteral("{\"id\":\"test.no_callback\",\"arguments\":{}}"),
		&nHttpStatus, 7000);
	if (nHttpStatus != 408 || timeout.value(QStringLiteral("isSuccess")).toBool(true)
		|| timeout.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("error")).toString() != QStringLiteral("command_timeout"))
	{
		return 11;
	}
	loader.shutdown();
	if (QFileInfo::exists(strDiscoveryPath))
		return 12;
	return 0;
}
