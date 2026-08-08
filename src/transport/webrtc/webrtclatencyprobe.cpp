#include "transport/webrtc/webrtclatencyprobe.h"

#include "common/latencytracelogger.h"
#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>

namespace
{
	constexpr char kLatencyPing[] = "latencyPing";
	constexpr char kLatencyPong[] = "latencyPong";
	constexpr char kLatencyId[] = "id";
	constexpr char kLatencySendMs[] = "sendMs";

	bool readLatencyFields(const QJsonObject &object, quint64 *pId, qint64 *pSendMs)
	{
		const QJsonValue idValue = object.value(QString::fromLatin1(kLatencyId));
		const QJsonValue sendMsValue = object.value(QString::fromLatin1(kLatencySendMs));
		if (!idValue.isString() || !sendMsValue.isDouble()
			|| sendMsValue.toDouble() != static_cast<double>(static_cast<qint64>(sendMsValue.toDouble())))
		{
			return false;
		}
		bool bOk = false;
		const quint64 nId = idValue.toString().toULongLong(&bOk);
		const qint64 nSendMs = static_cast<qint64>(sendMsValue.toDouble());
		if (!bOk || nId == 0 || nSendMs < 0)
			return false;
		*pId = nId;
		*pSendMs = nSendMs;
		return true;
	}
}

QString KWebRtcLatencyProbe::createPing()
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kLatencyId), QString::number(++m_nPingId));
	object.insert(QString::fromLatin1(kLatencySendMs), QDateTime::currentMSecsSinceEpoch());
	return KProtocolEnvelopeCodec::encode(InputProtocolChannel,
		QString::fromLatin1(kLatencyPing), QString(), 0, object);
}

bool KWebRtcLatencyProbe::handleMessage(const QString &strMessage,
	KSessionRole role,
	QString *pResponseMessage)
{
	if (pResponseMessage != nullptr)
		pResponseMessage->clear();
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(InputProtocolChannel,
		strMessage, &envelope, nullptr)
		|| envelope.nVersion != KProtocolConstraints::kEnvelopeSchemaVersion)
	{
		return false;
	}
	return handleMessage(envelope, role, pResponseMessage);
}

bool KWebRtcLatencyProbe::handleMessage(const KProtocolEnvelope &envelope,
	KSessionRole role,
	QString *pResponseMessage)
{
	if (pResponseMessage != nullptr)
		pResponseMessage->clear();
	const QJsonObject object = envelope.payload;
	const QString strType = envelope.strType;
	if (strType != QString::fromLatin1(kLatencyPing)
		&& strType != QString::fromLatin1(kLatencyPong))
	{
		return false;
	}
	quint64 nId = 0;
	qint64 nSendMs = -1;
	if (!readLatencyFields(object, &nId, &nSendMs))
		return false;
	if (strType == QString::fromLatin1(kLatencyPing))
	{
		if (role != ControlledSessionRole)
			return true;
		QJsonObject response;
		response.insert(QString::fromLatin1(kLatencyId),
			object.value(QString::fromLatin1(kLatencyId)));
		response.insert(QString::fromLatin1(kLatencySendMs),
			object.value(QString::fromLatin1(kLatencySendMs)));
		if (pResponseMessage != nullptr)
		{
			*pResponseMessage = KProtocolEnvelopeCodec::encode(InputProtocolChannel,
				QString::fromLatin1(kLatencyPong), QString(), 0, response);
		}
		return true;
	}
	if (strType != QString::fromLatin1(kLatencyPong))
		return false;
	if (role != ControllerSessionRole)
		return true;

	const qint64 nNowMs = QDateTime::currentMSecsSinceEpoch();
	if (nSendMs > nNowMs || nNowMs - nSendMs > 60000)
		return true;
	m_nRoundTripMs = static_cast<int>(nNowMs - nSendMs);
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("datachannel_rtt"),
		QStringLiteral("id=%1 rttMs=%2")
			.arg(nId)
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
