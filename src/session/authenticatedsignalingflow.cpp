#include "session/authenticatedsignalingflow.h"

#include "core/protocol/protocolenvelope.h"
#include "core/security/deviceidentityprovider.h"
#include "core/security/securitycanonicalwriter.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonObject>

namespace
{
	constexpr char kSenderDeviceId[] = "senderDeviceId";
	constexpr char kReceiverDeviceId[] = "receiverDeviceId";
	constexpr char kTranscriptHash[] = "transcriptHash";
	constexpr char kGeneration[] = "generation";
	constexpr char kPermissions[] = "permissions";
	constexpr char kSignalingPayload[] = "signaling";
	constexpr char kPayloadHash[] = "payloadHash";
	constexpr char kSignature[] = "signature";

	bool Fail(const QString &strMessage, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strMessage;
		return false;
	}

	bool DecodeBase64(const QJsonValue &value, int nExpectedSize,
		QByteArray *pBytes)
	{
		if (!value.isString())
			return false;
		const QByteArray encoded = value.toString().toLatin1();
		const auto result = QByteArray::fromBase64Encoding(encoded,
			QByteArray::AbortOnBase64DecodingErrors);
		if (!result || result.decoded.size() != nExpectedSize)
			return false;
		*pBytes = result.decoded;
		return true;
	}
}

KAuthenticatedSignalingFlow::KAuthenticatedSignalingFlow(
	KDeviceIdentityProvider *pIdentityProvider)
	: m_pIdentityProvider(pIdentityProvider)
{
	Q_ASSERT(m_pIdentityProvider != nullptr);
}

bool KAuthenticatedSignalingFlow::begin(
	const KDeviceAuthenticationContext &context,
	quint64 nLocalGeneration,
	QString *pErrorMessage)
{
	reset();
	const KDeviceIdentity identity = m_pIdentityProvider->identity();
	if (identity.strDeviceId.isEmpty()
		|| context.strRemoteDeviceId.isEmpty()
		|| context.strRequestId.isEmpty()
		|| context.transcriptHash.size() != 32
		|| !context.effectivePermissions.testFlag(ViewScreenPermissionScope)
		|| nLocalGeneration == 0)
	{
		return Fail(QStringLiteral("Incomplete authenticated signaling context"),
			pErrorMessage);
	}
	m_context = context;
	m_strLocalDeviceId = identity.strDeviceId;
	m_nLocalGeneration = nLocalGeneration;
	m_bReady = true;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

void KAuthenticatedSignalingFlow::reset()
{
	m_context = KDeviceAuthenticationContext();
	m_strLocalDeviceId.clear();
	m_nLocalGeneration = 0;
	m_nRemoteGeneration = 0;
	m_nSendSequence = 0;
	m_nReceiveSequence = 0;
	m_bReady = false;
}

bool KAuthenticatedSignalingFlow::isReady() const
{
	return m_bReady;
}

QString KAuthenticatedSignalingFlow::encode(const QString &strRawSignaling,
	QString *pErrorMessage)
{
	if (!m_bReady || strRawSignaling.isEmpty())
	{
		Fail(QStringLiteral("Authenticated signaling is not ready"), pErrorMessage);
		return QString();
	}
	const QByteArray payload = strRawSignaling.toUtf8();
	const QByteArray payloadHash = QCryptographicHash::hash(
		payload, QCryptographicHash::Sha256);
	const quint64 nSequence = ++m_nSendSequence;
	QByteArray signature;
	if (!m_pIdentityProvider->sign(signatureData(m_strLocalDeviceId,
		m_context.strRemoteDeviceId, m_nLocalGeneration, nSequence, payloadHash),
		&signature, pErrorMessage))
	{
		return QString();
	}
	QJsonObject object;
	object.insert(QString::fromLatin1(kSenderDeviceId), m_strLocalDeviceId);
	object.insert(QString::fromLatin1(kReceiverDeviceId),
		m_context.strRemoteDeviceId);
	object.insert(QString::fromLatin1(kTranscriptHash),
		QString::fromLatin1(m_context.transcriptHash.toBase64()));
	object.insert(QString::fromLatin1(kGeneration),
		QString::number(m_nLocalGeneration));
	object.insert(QString::fromLatin1(kPermissions),
		m_context.effectivePermissions.toInt());
	object.insert(QString::fromLatin1(kSignalingPayload),
		QString::fromLatin1(payload.toBase64()));
	object.insert(QString::fromLatin1(kPayloadHash),
		QString::fromLatin1(payloadHash.toBase64()));
	object.insert(QString::fromLatin1(kSignature),
		QString::fromLatin1(signature.toBase64()));
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return KProtocolEnvelopeCodec::encode(SignalingProtocolChannel,
		envelopeType(), m_context.strRequestId, nSequence, object);
}

bool KAuthenticatedSignalingFlow::decode(const KProtocolEnvelope &envelope,
	QString *pRawSignaling,
	QString *pErrorMessage)
{
	if (!m_bReady || pRawSignaling == nullptr
		|| envelope.strType != envelopeType()
		|| envelope.strRequestId != m_context.strRequestId)
	{
		return Fail(QStringLiteral("Authenticated signaling context mismatch"),
			pErrorMessage);
	}
	const QJsonObject object = envelope.payload;
	const QString strSender = object.value(QString::fromLatin1(kSenderDeviceId)).toString();
	const QString strReceiver = object.value(QString::fromLatin1(kReceiverDeviceId)).toString();
	bool bGenerationOk = false;
	const quint64 nGeneration = object.value(QString::fromLatin1(kGeneration))
		.toString().toULongLong(&bGenerationOk);
	const QJsonValue permissionsValue = object.value(QString::fromLatin1(kPermissions));
	if (strSender != m_context.strRemoteDeviceId
		|| strReceiver != m_strLocalDeviceId
		|| !bGenerationOk || nGeneration == 0
		|| !permissionsValue.isDouble()
		|| permissionsValue.toInt() != m_context.effectivePermissions.toInt()
		|| envelope.nSequence != m_nReceiveSequence + 1)
	{
		return Fail(QStringLiteral("Authenticated signaling metadata is invalid"),
			pErrorMessage);
	}
	if (m_nRemoteGeneration != 0 && nGeneration != m_nRemoteGeneration)
		return Fail(QStringLiteral("Authenticated signaling generation changed"), pErrorMessage);
	QByteArray transcriptHash;
	QByteArray payloadHash;
	QByteArray signature;
	if (!DecodeBase64(object.value(QString::fromLatin1(kTranscriptHash)), 32,
			&transcriptHash)
		|| transcriptHash != m_context.transcriptHash
		|| !DecodeBase64(object.value(QString::fromLatin1(kPayloadHash)), 32,
			&payloadHash)
		|| !DecodeBase64(object.value(QString::fromLatin1(kSignature)), 64,
			&signature)
		|| !object.value(QString::fromLatin1(kSignalingPayload)).isString())
	{
		return Fail(QStringLiteral("Authenticated signaling fields are invalid"),
			pErrorMessage);
	}
	const auto payloadResult = QByteArray::fromBase64Encoding(
		object.value(QString::fromLatin1(kSignalingPayload)).toString().toLatin1(),
		QByteArray::AbortOnBase64DecodingErrors);
	if (!payloadResult || payloadResult.decoded.isEmpty()
		|| QCryptographicHash::hash(payloadResult.decoded, QCryptographicHash::Sha256)
			!= payloadHash
		|| !m_pIdentityProvider->verify(m_context.remotePublicKey,
			signatureData(strSender, strReceiver, nGeneration,
				envelope.nSequence, payloadHash), signature, pErrorMessage))
	{
		return Fail(QStringLiteral("Authenticated signaling signature is invalid"),
			pErrorMessage);
	}
	m_nRemoteGeneration = nGeneration;
	m_nReceiveSequence = envelope.nSequence;
	*pRawSignaling = QString::fromUtf8(payloadResult.decoded);
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KAuthenticatedSignalingFlow::envelopeType()
{
	return QStringLiteral("authenticatedSignaling");
}

QByteArray KAuthenticatedSignalingFlow::signatureData(
	const QString &strSenderDeviceId,
	const QString &strReceiverDeviceId,
	quint64 nGeneration,
	quint64 nSequence,
	const QByteArray &payloadHash) const
{
	KSecurityCanonicalWriter writer;
	writer.appendString(QStringLiteral("wrc.authenticated-signaling"));
	writer.appendUInt32(1);
	writer.appendString(m_context.strRequestId);
	writer.appendString(strSenderDeviceId);
	writer.appendString(strReceiverDeviceId);
	writer.appendBytes(m_context.transcriptHash);
	writer.appendUInt64(nGeneration);
	writer.appendUInt64(nSequence);
	writer.appendUInt32(static_cast<quint32>(
		m_context.effectivePermissions.toInt()));
	writer.appendBytes(payloadHash);
	return writer.data();
}
