#include "automation/driver/base/httpserver.h"

#include "automation/driver/base/status.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

#include <cstring>

namespace
{
	constexpr int kMaximumHeaderBytes = 16 * 1024;
	constexpr int kMaximumBodyBytes = 16 * 1024;
	constexpr int kMaximumConnections = 32;

	QByteArray StatusReason(int nStatusCode)
	{
		if (nStatusCode == 200)
			return QByteArrayLiteral("OK");
		if (nStatusCode == 400)
			return QByteArrayLiteral("Bad Request");
		if (nStatusCode == 401)
			return QByteArrayLiteral("Unauthorized");
		if (nStatusCode == 404)
			return QByteArrayLiteral("Not Found");
		if (nStatusCode == 408)
			return QByteArrayLiteral("Request Timeout");
		if (nStatusCode == 413)
			return QByteArrayLiteral("Payload Too Large");
		if (nStatusCode == 503)
			return QByteArrayLiteral("Service Unavailable");
		return QByteArrayLiteral("Internal Server Error");
	}

	QByteArray RandomToken()
	{
		QByteArray bytes(32, Qt::Uninitialized);
		for (int nOffset = 0; nOffset < bytes.size(); nOffset += 4)
		{
			const quint32 nValue = QRandomGenerator::system()->generate();
			const int nCopyBytes = qMin(4, bytes.size() - nOffset);
			memcpy(bytes.data() + nOffset, &nValue, static_cast<size_t>(nCopyBytes));
		}
		return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	}
}

KHttpServer::KHttpServer(QObject *pParent)
	: QObject(pParent)
{
	connect(&m_server, &QTcpServer::newConnection, this, &KHttpServer::handleNewConnection);
}

KHttpServer::~KHttpServer()
{
	stop();
}

bool KHttpServer::start(const QString &strDiscoveryDirectory,
	qint64 nPid,
	const QString &strBuildId,
	QString *pErrorMessage)
{
	if (m_server.isListening())
		return true;
	m_strToken = QString::fromLatin1(RandomToken());
	if (!m_server.listen(QHostAddress::LocalHost, 0))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = m_server.errorString();
		return false;
	}
	if (!writeDiscoveryFile(strDiscoveryDirectory, nPid, strBuildId, pErrorMessage))
	{
		m_server.close();
		return false;
	}
	return true;
}

void KHttpServer::stop()
{
	m_server.close();
	for (QTcpSocket *pSocket : m_connections.keys())
	{
		pSocket->disconnect(this);
		pSocket->abort();
		pSocket->deleteLater();
	}
	m_connections.clear();
	m_pendingRequests.clear();
	removeDiscoveryFile();
	m_strToken.clear();
}

quint16 KHttpServer::port() const
{
	return m_server.serverPort();
}

QString KHttpServer::token() const
{
	return m_strToken;
}

void KHttpServer::sendJsonResponse(quint64 nRequestId,
	int nStatusCode,
	const QJsonObject &response)
{
	QTcpSocket *pSocket = m_pendingRequests.take(nRequestId);
	if (pSocket == nullptr)
		return;
	const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
	QByteArray header = QByteArrayLiteral("HTTP/1.1 ")
		+ QByteArray::number(nStatusCode) + ' ' + StatusReason(nStatusCode)
		+ QByteArrayLiteral("\r\nContent-Type: application/json; charset=utf-8\r\n")
		+ QByteArrayLiteral("Content-Length: ") + QByteArray::number(body.size())
		+ QByteArrayLiteral("\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
	pSocket->write(header);
	pSocket->write(body);
	pSocket->flush();
	pSocket->waitForBytesWritten(100);
	pSocket->disconnectFromHost();
}

void KHttpServer::handleNewConnection()
{
	while (m_server.hasPendingConnections())
	{
		QTcpSocket *pSocket = m_server.nextPendingConnection();
		if (m_connections.size() >= kMaximumConnections)
		{
			reject(pSocket, 503, QStringLiteral("too_many_connections"));
			continue;
		}
		KConnection connection;
		connection.pReadTimer = new QTimer(pSocket);
		connection.pReadTimer->setSingleShot(true);
		connection.pReadTimer->setInterval(5000);
		connect(connection.pReadTimer, &QTimer::timeout, this,
			[this, pSocket]()
			{
				if (m_connections.contains(pSocket))
					reject(pSocket, 408, QStringLiteral("request_timeout"));
			});
		connection.pReadTimer->start();
		m_connections.insert(pSocket, connection);
		connect(pSocket, &QTcpSocket::readyRead, this,
			[this, pSocket]() { handleReadyRead(pSocket); });
		connect(pSocket, &QTcpSocket::disconnected, this,
			[this, pSocket]()
			{
				m_connections.remove(pSocket);
				for (auto iter = m_pendingRequests.begin(); iter != m_pendingRequests.end();)
				{
					if (iter.value() == pSocket)
						iter = m_pendingRequests.erase(iter);
					else
						++iter;
				}
				pSocket->deleteLater();
			});
	}
}

void KHttpServer::handleReadyRead(QTcpSocket *pSocket)
{
	auto iter = m_connections.find(pSocket);
	if (iter == m_connections.end())
		return;
	iter->buffer.append(pSocket->readAll());
	if (iter->buffer.size() > kMaximumHeaderBytes + kMaximumBodyBytes)
	{
		reject(pSocket, 413, QStringLiteral("request_too_large"));
		return;
	}
	tryParseRequest(pSocket, &iter.value());
}

bool KHttpServer::tryParseRequest(QTcpSocket *pSocket, KConnection *pConnection)
{
	if (pConnection->bHeadersParsed)
		return false;
	const qsizetype nHeaderEnd = pConnection->buffer.indexOf(QByteArrayLiteral("\r\n\r\n"));
	if (nHeaderEnd < 0)
	{
		if (pConnection->buffer.size() > kMaximumHeaderBytes)
			reject(pSocket, 413, QStringLiteral("headers_too_large"));
		return false;
	}
	const QList<QByteArray> lines = pConnection->buffer.left(nHeaderEnd).split('\n');
	if (lines.isEmpty())
	{
		reject(pSocket, 400, QStringLiteral("malformed_request"));
		return false;
	}
	const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
	if (requestLine.size() != 3 || requestLine.at(2) != QByteArrayLiteral("HTTP/1.1"))
	{
		reject(pSocket, 400, QStringLiteral("malformed_request_line"));
		return false;
	}
	qint64 nContentLength = 0;
	QByteArray token;
	for (int i = 1; i < lines.size(); ++i)
	{
		const QByteArray line = lines.at(i).trimmed();
		const qsizetype nColon = line.indexOf(':');
		if (nColon <= 0)
			continue;
		const QByteArray name = line.left(nColon).trimmed().toLower();
		const QByteArray value = line.mid(nColon + 1).trimmed();
		if (name == QByteArrayLiteral("content-length"))
		{
			bool bOk = false;
			nContentLength = value.toLongLong(&bOk);
			if (!bOk || nContentLength < 0 || nContentLength > kMaximumBodyBytes)
			{
				reject(pSocket, 413, QStringLiteral("invalid_content_length"));
				return false;
			}
		}
		else if (name == QByteArrayLiteral("x-wrc-token"))
		{
			token = value;
		}
	}
	if (token != m_strToken.toUtf8())
	{
		reject(pSocket, 401, QStringLiteral("invalid_token"));
		return false;
	}
	const qsizetype nBodyOffset = nHeaderEnd + 4;
	if (pConnection->buffer.size() - nBodyOffset < nContentLength)
		return false;

	const quint64 nRequestId = m_nNextRequestId++;
	pConnection->bHeadersParsed = true;
	if (pConnection->pReadTimer != nullptr)
		pConnection->pReadTimer->stop();
	m_pendingRequests.insert(nRequestId, pSocket);
	emit requestReceived(nRequestId, requestLine.at(0), requestLine.at(1),
		pConnection->buffer.mid(nBodyOffset, nContentLength));
	return true;
}

void KHttpServer::reject(QTcpSocket *pSocket, int nStatusCode, const QString &strError)
{
	if (pSocket == nullptr)
		return;
	if (!m_connections.contains(pSocket))
	{
		connect(pSocket, &QTcpSocket::disconnected,
			pSocket, &QObject::deleteLater);
	}
	const QByteArray body = QJsonDocument(DriverErrorResponse(
		InvalidArgumentDriverStatus, strError, strError)).toJson(QJsonDocument::Compact);
	const QByteArray response = QByteArrayLiteral("HTTP/1.1 ")
		+ QByteArray::number(nStatusCode) + ' ' + StatusReason(nStatusCode)
		+ QByteArrayLiteral("\r\nContent-Type: application/json\r\nContent-Length: ")
		+ QByteArray::number(body.size()) + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
		+ body;
	pSocket->write(response);
	pSocket->disconnectFromHost();
}

bool KHttpServer::writeDiscoveryFile(const QString &strDiscoveryDirectory,
	qint64 nPid,
	const QString &strBuildId,
	QString *pErrorMessage)
{
	if (!QDir().mkpath(strDiscoveryDirectory))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to create automation discovery directory");
		return false;
	}
	m_strDiscoveryFilePath = QDir(strDiscoveryDirectory)
		.filePath(QStringLiteral("%1.json").arg(nPid));
	QSaveFile file(m_strDiscoveryFilePath);
	if (!file.open(QIODevice::WriteOnly))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = file.errorString();
		return false;
	}
	const QJsonObject object{
		{QStringLiteral("pid"), QString::number(nPid)},
		{QStringLiteral("port"), m_server.serverPort()},
		{QStringLiteral("token"), m_strToken},
		{QStringLiteral("protocolVersion"), 1},
		{QStringLiteral("buildId"), strBuildId},
		{QStringLiteral("startedAtMs"), QString::number(QDateTime::currentMSecsSinceEpoch())}
	};
	if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0
		|| !file.commit())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = file.errorString();
		return false;
	}
	return true;
}

void KHttpServer::removeDiscoveryFile()
{
	if (!m_strDiscoveryFilePath.isEmpty())
		QFile::remove(m_strDiscoveryFilePath);
	m_strDiscoveryFilePath.clear();
}
