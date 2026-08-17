#ifndef _WINREMOTECONTROL_SESSION_TRUSTEDDEVICESERVICE_H_
#define _WINREMOTECONTROL_SESSION_TRUSTEDDEVICESERVICE_H_

#include "core/security/trusteddevice.h"
#include "core/transport/tlspeeridentity.h"

class KTrustedDeviceStore;

class KTrustedDeviceService
{
public:
	explicit KTrustedDeviceService(KTrustedDeviceStore *pStore);

	bool load(QString *pErrorMessage);
	const KTrustedDevice *find(const QString &strDeviceId) const;
	QString mutualCommitId(const QString &strDeviceId,
		const QByteArray &spkiSha256) const;
	bool isMutuallyTrusted(const QString &strDeviceId,
		const QByteArray &spkiSha256,
		const QString &strPeerCommitId) const;
	bool prepare(const QString &strRequestId,
		const KTlsPeerIdentity &peer,
		const QString &strDeviceName,
		KPermissionScopes permissions,
		QString *pErrorMessage);
	bool commit(const QString &strRequestId, QString *pErrorMessage);
	bool rollback(const QString &strRequestId, QString *pErrorMessage);
	bool updateAuthenticated(const QString &strDeviceId,
		const QString &strDeviceName,
		const QByteArray &certificateSha256,
		QString *pErrorMessage);
	void complete(const QString &strRequestId);
	bool hasTransaction(const QString &strRequestId) const;

private:
	KTrustedDevice *findMutable(const QString &strDeviceId);
	bool saveCurrent(QString *pErrorMessage);

	KTrustedDeviceStore *m_pStore = nullptr;
	QVector<KTrustedDevice> m_devices;
	QVector<KTrustedDevice> m_beforeTransaction;
	QString m_strTransactionId;
	QString m_strTransactionDeviceId;
};

#endif // _WINREMOTECONTROL_SESSION_TRUSTEDDEVICESERVICE_H_
