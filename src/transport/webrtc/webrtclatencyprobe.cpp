#include "transport/webrtc/webrtclatencyprobe.h"

#include "common/latencytracelogger.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace
{
	constexpr char kMessageType[] = "type";
	constexpr char kLatencyPing[] = "latencyPing";
	constexpr char kLatencyPong[] = "latencyPong";
	constexpr char kLatencyId[] = "id";
	constexpr char kLatencySendMs[] = "sendMs";
}

QString KWebRtcLatencyProbe::createPing()
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kMessageType), QString::fromLatin1(kLatencyPing));
	object.insert(QString::fromLatin1(kLatencyId), QString::number(++m_nPingId));
	object.insert(QString::fromLatin1(kLatencySendMs), QDateTime::currentMSecsSinceEpoch());
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool KWebRtcLatencyProbe::handleMessage(const QString &strMessage,
	KSessionRole role,
	QString *pResponseMessage)
{
	if (pResponseMessage != nullptr)
		pResponseMessage->clear();
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	if (!document.isObject())
		return false;

	const QJsonObject object = document.object();
	const QString strType = object.value(QString::fromLatin1(kMessageType)).toString();
	if (strType == QString::fromLatin1(kLatencyPing))
	{
		if (role != ControlledSessionRole)
			return true;
		QJsonObject response;
		response.insert(QString::fromLatin1(kMessageType), QString::fromLatin1(kLatencyPong));
		response.insert(QString::fromLatin1(kLatencyId),
			object.value(QString::fromLatin1(kLatencyId)));
		response.insert(QString::fromLatin1(kLatencySendMs),
			object.value(QString::fromLatin1(kLatencySendMs)));
		if (pResponseMessage != nullptr)
		{
			*pResponseMessage = QString::fromUtf8(
				QJsonDocument(response).toJson(QJsonDocument::Compact));
		}
		return true;
	}
	if (strType != QString::fromLatin1(kLatencyPong))
		return false;
	if (role != ControllerSessionRole)
		return true;

	const qint64 nSendMs = static_cast<qint64>(
		object.value(QString::fromLatin1(kLatencySendMs)).toDouble(-1));
	if (nSendMs < 0)
		return true;
	m_nRoundTripMs = static_cast<int>(QDateTime::currentMSecsSinceEpoch() - nSendMs);
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("datachannel_rtt"),
		QStringLiteral("id=%1 rttMs=%2")
			.arg(object.value(QString::fromLatin1(kLatencyId)).toString())
			.arg(m_nRoundTripMs));
	return true;
}

void KWebRtcLatencyProbe::reset()
{
	m_nRoundTripMs = -1;
}

int KWebRtcLatencyProbe::roundTripMs() const
{
	return m_nRoundTripMs;
}
