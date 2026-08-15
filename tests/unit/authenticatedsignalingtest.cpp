#include "fakesecurity.h"
#include "session/authenticatedsignalingflow.h"

#include "core/protocol/protocolenvelope.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>
#include <QtCore/QUuid>

namespace
{
	bool Require(bool bCondition, const QString &strMessage)
	{
		if (bCondition)
			return true;
		QTextStream(stderr) << strMessage << Qt::endl;
		return false;
	}

	KDeviceAuthenticationContext ContextFor(
		const KFakeDeviceIdentityProvider &remote,
		const QString &strRequestId,
		const QByteArray &transcriptHash)
	{
		KDeviceAuthenticationContext context;
		context.strRequestId = strRequestId;
		context.strRemoteDeviceId = remote.identity().strDeviceId;
		context.remotePublicKey = remote.identity().publicKey;
		context.strRemoteFingerprint = remote.identity().strFingerprint;
		context.transcriptHash = transcriptHash;
		context.effectivePermissions = KPermissionScopes::fromInt(
			kAllPermissionScopeBits);
		return context;
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	KFakeDeviceIdentityProvider controllerIdentity;
	KFakeDeviceIdentityProvider controlledIdentity;
	KAuthenticatedSignalingFlow controller(&controllerIdentity);
	KAuthenticatedSignalingFlow controlled(&controlledIdentity);
	const QString strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const QByteArray transcriptHash(32, '\x5a');
	QString strError;
	if (!Require(controller.begin(ContextFor(controlledIdentity,
		strRequestId, transcriptHash), 9, &strError), strError)
		|| !Require(controlled.begin(ContextFor(controllerIdentity,
			strRequestId, transcriptHash), 41, &strError), strError))
	{
		return 1;
	}

	const QString strRawOffer = QStringLiteral(
		"{\"channel\":\"signaling\",\"payload\":{\"sdp\":\"v=0\","
		"\"sdpType\":\"offer\"},\"requestId\":\"\",\"sequence\":\"0\","
		"\"type\":\"offer\",\"version\":1}");
	const QString strEncoded = controller.encode(strRawOffer, &strError);
	KProtocolEnvelope envelope;
	QString strDecoded;
	if (!Require(!strEncoded.isEmpty(), strError)
		|| !Require(KProtocolEnvelopeCodec::decode(SignalingProtocolChannel,
			strEncoded, &envelope, &strError), strError)
		|| !Require(controlled.decode(envelope, &strDecoded, &strError)
			&& strDecoded == strRawOffer,
			QStringLiteral("Authenticated signaling did not round trip"))
		|| !Require(!controlled.decode(envelope, &strDecoded, &strError),
			QStringLiteral("Authenticated signaling replay was accepted")))
	{
		return 1;
	}

	const QString strSecond = controller.encode(strRawOffer, &strError);
	KProtocolEnvelope tampered;
	if (!Require(KProtocolEnvelopeCodec::decode(SignalingProtocolChannel,
		strSecond, &tampered, &strError), strError))
	{
		return 1;
	}
	QJsonObject payload = tampered.payload;
	payload.insert(QStringLiteral("permissions"), ViewScreenPermissionScope);
	const QString strTampered = KProtocolEnvelopeCodec::encode(
		SignalingProtocolChannel, tampered.strType, tampered.strRequestId,
		tampered.nSequence, payload);
	if (!Require(KProtocolEnvelopeCodec::decode(SignalingProtocolChannel,
		strTampered, &tampered, &strError), strError)
		|| !Require(!controlled.decode(tampered, &strDecoded, &strError),
			QStringLiteral("Tampered signaling permissions were accepted")))
	{
		return 1;
	}
	return 0;
}
