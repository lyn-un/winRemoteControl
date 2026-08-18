#ifndef _WINREMOTECONTROL_ADAPTERS_SIGNALING_SCHANNELTLSENGINE_H_
#define _WINREMOTECONTROL_ADAPTERS_SIGNALING_SCHANNELTLSENGINE_H_

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS
#endif

#include "core/transport/tlspeeridentity.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

#include <windows.h>
#include <winternl.h>
#include <security.h>
#include <schannel.h>

class KSchannelTlsEngine final
{
public:
	KSchannelTlsEngine();
	~KSchannelTlsEngine();

	KSchannelTlsEngine(const KSchannelTlsEngine &) = delete;
	KSchannelTlsEngine &operator=(const KSchannelTlsEngine &) = delete;

	bool initialize(bool bServer, void *pCertificateContext, QString *pErrorMessage);
	bool start(QList<QByteArray> *pOutputRecords, QString *pErrorMessage);
	bool continueHandshake(QByteArray *pEncryptedBuffer,
		QList<QByteArray> *pOutputRecords,
		bool *pCompleted,
		QString *pErrorMessage);
	bool encrypt(const QByteArray &plainText,
		QList<QByteArray> *pOutputRecords,
		QString *pErrorMessage);
	bool decrypt(QByteArray *pEncryptedBuffer,
		QList<QByteArray> *pPlainTexts,
		bool *pClosed,
		QString *pErrorMessage);
	bool shutdown(QList<QByteArray> *pOutputRecords,
		QString *pErrorMessage);
	bool peerIdentity(const QString &strSourceAddress,
		KTlsPeerIdentity *pIdentity,
		QString *pErrorMessage) const;
	bool exportKeyingMaterial(const QByteArray &label,
		const QByteArray &context,
		int nLength,
		QByteArray *pKeyingMaterial,
		QString *pErrorMessage);
	bool isReady() const;
	void clear();

private:
	bool handshakeStep(QByteArray *pEncryptedBuffer,
		bool bInitial,
		QList<QByteArray> *pOutputRecords,
		bool *pCompleted,
		QString *pErrorMessage);
	bool finishHandshake(QString *pErrorMessage);

	CredHandle m_credential = {};
	CtxtHandle m_context = {};
	PCCERT_CONTEXT m_pLocalCertificate = nullptr;
	SecPkgContext_StreamSizes m_streamSizes = {};
	bool m_bServer = false;
	bool m_bCredentialValid = false;
	bool m_bContextValid = false;
	bool m_bReady = false;
};

#endif // _WINREMOTECONTROL_ADAPTERS_SIGNALING_SCHANNELTLSENGINE_H_
