#ifndef _WINREMOTECONTROL_SESSION_AUTHENTICATEDSIGNALINGFLOW_H_
#define _WINREMOTECONTROL_SESSION_AUTHENTICATEDSIGNALINGFLOW_H_

#include "session/deviceauthenticationflow.h"

#include <QtCore/QString>

class KDeviceIdentityProvider;
struct KProtocolEnvelope;

class KAuthenticatedSignalingFlow
{
public:
	explicit KAuthenticatedSignalingFlow(
		KDeviceIdentityProvider *pIdentityProvider);

	bool begin(const KDeviceAuthenticationContext &context,
		quint64 nLocalGeneration,
		QString *pErrorMessage);
	void reset();
	bool isReady() const;
	QString encode(const QString &strRawSignaling,
		QString *pErrorMessage);
	bool decode(const KProtocolEnvelope &envelope,
		QString *pRawSignaling,
		QString *pErrorMessage);
	static QString envelopeType();

private:
	QByteArray signatureData(const QString &strSenderDeviceId,
		const QString &strReceiverDeviceId,
		quint64 nGeneration,
		quint64 nSequence,
		const QByteArray &payloadHash) const;

	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	KDeviceAuthenticationContext m_context;
	QString m_strLocalDeviceId;
	quint64 m_nLocalGeneration = 0;
	quint64 m_nRemoteGeneration = 0;
	quint64 m_nSendSequence = 0;
	quint64 m_nReceiveSequence = 0;
	bool m_bReady = false;
};

#endif // _WINREMOTECONTROL_SESSION_AUTHENTICATEDSIGNALINGFLOW_H_
