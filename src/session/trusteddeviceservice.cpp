#include "session/trusteddeviceservice.h"

#include "core/security/trusteddevicestore.h"

#include <QtCore/QDateTime>
#include <QtCore/QUuid>

KTrustedDeviceService::KTrustedDeviceService(KTrustedDeviceStore *pStore)
	: m_pStore(pStore)
{
	Q_ASSERT(m_pStore != nullptr);
}

bool KTrustedDeviceService::load(QString *pErrorMessage)
{
	QString strError;
	QVector<KTrustedDevice> devices = m_pStore->loadDevices(&strError);
	if (!strError.isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}
	bool bRemovedPending = false;
	for (qsizetype nIndex = devices.size() - 1; nIndex >= 0; --nIndex)
	{
		if (devices.at(nIndex).commitState == PendingTrustedDeviceCommitState)
		{
			devices.removeAt(nIndex);
			bRemovedPending = true;
		}
	}
	if (bRemovedPending && !m_pStore->saveDevices(devices, &strError))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}
	m_devices = devices;
	m_beforeTransaction.clear();
	m_strTransactionId.clear();
	m_strTransactionDeviceId.clear();
	return true;
}

const KTrustedDevice *KTrustedDeviceService::find(
	const QString &strDeviceId) const
{
	for (const KTrustedDevice &device : m_devices)
	{
		if (device.strDeviceId == strDeviceId)
			return &device;
	}
	return nullptr;
}

QString KTrustedDeviceService::mutualCommitId(const QString &strDeviceId,
	const QByteArray &spkiSha256) const
{
	const KTrustedDevice *pDevice = find(strDeviceId);
	if (pDevice == nullptr
		|| pDevice->commitState != MutualTrustedDeviceCommitState
		|| pDevice->spkiSha256 != spkiSha256
		|| QUuid(pDevice->strPairingTransactionId).isNull())
	{
		return QString();
	}
	return pDevice->strPairingTransactionId;
}

bool KTrustedDeviceService::isMutuallyTrusted(const QString &strDeviceId,
	const QByteArray &spkiSha256,
	const QString &strPeerCommitId) const
{
	const QString strLocalCommitId = mutualCommitId(strDeviceId, spkiSha256);
	return !strLocalCommitId.isEmpty() && strLocalCommitId == strPeerCommitId;
}

bool KTrustedDeviceService::prepare(const QString &strRequestId,
	const KTlsPeerIdentity &peer,
	const QString &strDeviceName,
	KPermissionScopes permissions,
	QString *pErrorMessage)
{
	if (QUuid(strRequestId).isNull() || !peer.isValid()
		|| !permissions.testFlag(ViewScreenPermissionScope)
		|| !m_strTransactionId.isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Invalid or overlapping pairing transaction");
		return false;
	}
	m_beforeTransaction = m_devices;
	m_strTransactionId = strRequestId;
	m_strTransactionDeviceId = peer.strDeviceId;
	KTrustedDevice *pDevice = findMutable(peer.strDeviceId);
	if (pDevice == nullptr)
	{
		KTrustedDevice device;
		device.strDeviceId = peer.strDeviceId;
		m_devices.append(device);
		pDevice = &m_devices.last();
	}
	const qint64 nNowMs = QDateTime::currentMSecsSinceEpoch();
	pDevice->spkiSha256 = peer.spkiSha256;
	pDevice->certificateSha256 = peer.certificateSha256;
	pDevice->strFingerprint = peer.spkiFingerprint();
	pDevice->strAlias = strDeviceName;
	pDevice->strAdvertisedName = strDeviceName;
	pDevice->permissionLimit = permissions;
	if (pDevice->nPairedAtMs == 0)
		pDevice->nPairedAtMs = nNowMs;
	pDevice->nLastAuthenticatedAtMs = nNowMs;
	pDevice->bRevoked = false;
	pDevice->commitState = PendingTrustedDeviceCommitState;
	pDevice->strPairingTransactionId = strRequestId;
	if (saveCurrent(pErrorMessage))
		return true;
	m_devices = m_beforeTransaction;
	m_beforeTransaction.clear();
	m_strTransactionId.clear();
	m_strTransactionDeviceId.clear();
	return false;
}

bool KTrustedDeviceService::commit(const QString &strRequestId,
	QString *pErrorMessage)
{
	if (!hasTransaction(strRequestId))
		return false;
	KTrustedDevice *pDevice = findMutable(m_strTransactionDeviceId);
	if (pDevice == nullptr
		|| pDevice->commitState != PendingTrustedDeviceCommitState
		|| pDevice->strPairingTransactionId != strRequestId)
	{
		return false;
	}
	pDevice->commitState = MutualTrustedDeviceCommitState;
	if (saveCurrent(pErrorMessage))
		return true;
	pDevice->commitState = PendingTrustedDeviceCommitState;
	return false;
}

bool KTrustedDeviceService::rollback(const QString &strRequestId,
	QString *pErrorMessage)
{
	if (!hasTransaction(strRequestId))
		return true;
	const QVector<KTrustedDevice> failedState = m_devices;
	m_devices = m_beforeTransaction;
	if (!saveCurrent(pErrorMessage))
	{
		m_devices = failedState;
		return false;
	}
	complete(strRequestId);
	return true;
}

bool KTrustedDeviceService::updateAuthenticated(const QString &strDeviceId,
	const QString &strDeviceName,
	const QByteArray &certificateSha256,
	QString *pErrorMessage)
{
	KTrustedDevice *pDevice = findMutable(strDeviceId);
	if (pDevice == nullptr)
		return false;
	const QVector<KTrustedDevice> previous = m_devices;
	pDevice->strAdvertisedName = strDeviceName;
	pDevice->certificateSha256 = certificateSha256;
	pDevice->nLastAuthenticatedAtMs = QDateTime::currentMSecsSinceEpoch();
	if (saveCurrent(pErrorMessage))
		return true;
	m_devices = previous;
	return false;
}

void KTrustedDeviceService::complete(const QString &strRequestId)
{
	if (!hasTransaction(strRequestId))
		return;
	m_beforeTransaction.clear();
	m_strTransactionId.clear();
	m_strTransactionDeviceId.clear();
}

bool KTrustedDeviceService::hasTransaction(const QString &strRequestId) const
{
	return !m_strTransactionId.isEmpty()
		&& strRequestId == m_strTransactionId;
}

KTrustedDevice *KTrustedDeviceService::findMutable(
	const QString &strDeviceId)
{
	for (KTrustedDevice &device : m_devices)
	{
		if (device.strDeviceId == strDeviceId)
			return &device;
	}
	return nullptr;
}

bool KTrustedDeviceService::saveCurrent(QString *pErrorMessage)
{
	return m_pStore->saveDevices(m_devices, pErrorMessage);
}
