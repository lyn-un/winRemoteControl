#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"
#include "core/protocol/protocolrouter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>

#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const char *pDescription)
	{
		if (bCondition)
			return;
		std::cerr << "FAILED: " << pDescription << '\n';
		++g_nFailureCount;
	}

	void TestRandomPayloadBoundaries()
	{
		QRandomGenerator random(0x5a17c0de);
		KProtocolRouter router;
		KProtocolRouteContext context;
		for (int nIndex = 0; nIndex < 2000; ++nIndex)
		{
			const int nLength = random.bounded(4097);
			QByteArray bytes(nLength, Qt::Uninitialized);
			for (int nByteIndex = 0; nByteIndex < nLength; ++nByteIndex)
				bytes[nByteIndex] = static_cast<char>(random.bounded(128));
			const QString strMessage = QString::fromUtf8(bytes);
			KProtocolEnvelope envelope;
			KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
				strMessage, &envelope, nullptr);
			router.route(SessionProtocolChannel, strMessage, context);
		}
		Check(true, "random protocol payloads are handled without failure");
	}

	void TestKnownVersionIgnoresUnknownFields()
	{
		QJsonObject payload;
		payload.insert(QStringLiteral("futurePayloadField"), 42);
		QJsonObject object;
		object.insert(QStringLiteral("version"),
			KProtocolConstraints::kEnvelopeSchemaVersion);
		object.insert(QStringLiteral("type"), QStringLiteral("futureType"));
		object.insert(QStringLiteral("futureEnvelopeField"), true);
		object.insert(QStringLiteral("payload"), payload);
		KProtocolEnvelope envelope;
		QString strError;
		Check(KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
			QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)),
			&envelope, &strError),
			"known schema versions ignore unknown fields");
		Check(envelope.payload.value(QStringLiteral("futurePayloadField")).toInt() == 42,
			"unknown payload fields remain available to newer handlers");
	}

	void TestOversizedEnvelopeRejected()
	{
		const QString strOversized(
			KProtocolConstraints::kMaximumSignalingMessageBytes + 1, QLatin1Char('x'));
		KProtocolEnvelope envelope;
		Check(!KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
			strOversized, &envelope, nullptr),
			"oversized protocol messages are rejected");
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestRandomPayloadBoundaries();
	TestKnownVersionIgnoresUnknownFields();
	TestOversizedEnvelopeRejected();
	return g_nFailureCount == 0 ? 0 : 1;
}
