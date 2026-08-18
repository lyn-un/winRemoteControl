#include "session/trusteddeviceservice.h"

#include "core/security/trusteddevicestore.h"

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QUuid>
#include <utility>

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
	bool bRecoveredTransaction = false;
	for (qsizetype nIndex = devices.size() - 1; nIndex >= 0; --nIndex)
	{
		if (devices.at(nIndex).commitState == PendingTrustedDeviceCommitState)
		{
			devices.removeAt(nIndex);
			bRecoveredTransaction = true;
		}
	}
	QHash<QString, qsizetype> oldestMutualByDevice;
	for (qsizetype nIndex = 0; nIndex < devices.size(); ++nIndex)
	{
		const KTrustedDevice &device = devices.at(nIndex);
		const auto iterator = oldestMutualByDevice.constFind(device.strDeviceId);
		if (iterator == oldestMutualByDevice.constEnd())
		{
			oldestMutualByDevice.insert(device.strDeviceId, nIndex);
			continue;
		}
		const KTrustedDevice &oldest = devices.at(iterator.value());
		if (device.nPairedAtMs < oldest.nPairedAtMs
			|| (device.nPairedAtMs == oldest.nPairedAtMs
				&& device.strPairingTransactionId
					< oldest.strPairingTransactionId))
		{
			oldestMutualByDevice.insert(device.strDeviceId, nIndex);
		}
	}
	for (qsizetype nIndex = devices.size() - 1; nIndex >= 0; --nIndex)
	{
		if (oldestMutualByDevice.value(devices.at(nIndex).strDeviceId)
			!= nIndex)
		{
			devices.removeAt(nIndex);
			bRecoveredTransaction = true;
		}
	}
	if (bRecoveredTransaction && !m_pStore->saveDevices(devices, &strError))
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

KTrustedDeviceStoreError KTrustedDeviceService::lastLoadError() const
{
	return m_pStore->lastLoadError();
}

const KTrustedDevice *KTrustedDeviceService::find(
	const QString &strDeviceId) const
{
	const KTrustedDevice *pPending = nullptr;
	for (const KTrustedDevice &device : m_devices)
	{
		if (device.strDeviceId != strDeviceId)
			continue;
		if (device.commitState == MutualTrustedDeviceCommitState)
			return &device;
		pPending = &device;
	}
	return pPending;
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
	KTrustedDevice device;
	device.strDeviceId = peer.strDeviceId;
	m_devices.append(device);
	KTrustedDevice *pDevice = &m_devices.last();
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
	const QString &strDeviceName,
	const QByteArray &certificateSha256,
	QString *pErrorMessage)
{
	if (!hasTransaction(strRequestId))
		return false;
	KTrustedDevice *pDevice = findTransactionMutable(strRequestId);
	if (pDevice == nullptr
		|| pDevice->commitState != PendingTrustedDeviceCommitState
		|| pDevice->strPairingTransactionId != strRequestId)
	{
		return false;
	}
	pDevice->strAdvertisedName = strDeviceName;
	pDevice->certificateSha256 = certificateSha256;
	pDevice->nLastAuthenticatedAtMs = QDateTime::currentMSecsSinceEpoch();
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
	clearTransaction();
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

bool KTrustedDeviceService::complete(const QString &strRequestId,
	QString *pErrorMessage)
{
	if (!hasTransaction(strRequestId))
		return true;
	QVector<KTrustedDevice> completedDevices;
	completedDevices.reserve(m_devices.size());
	for (const KTrustedDevice &device : std::as_const(m_devices))
	{
		if (device.strDeviceId == m_strTransactionDeviceId
			&& device.strPairingTransactionId != strRequestId)
		{
			continue;
		}
		completedDevices.append(device);
	}
	const bool bCleanupRequired = completedDevices.size() != m_devices.size();
	if (bCleanupRequired)
	{
		// Completion cleanup is deliberately best-effort. The old Mutual record
		// remains a durable rollback point until both peers exchanged Committed.
		// If this save fails, the current session may proceed; startup recovery
		// will select the older, previously confirmed Mutual record.
		m_devices = completedDevices;
	}
	clearTransaction();
	return !bCleanupRequired || saveCurrent(pErrorMessage);
}

bool KTrustedDeviceService::hasTransaction(const QString &strRequestId) const
{
	return !m_strTransactionId.isEmpty()
		&& strRequestId == m_strTransactionId;
}

KTrustedDevice *KTrustedDeviceService::findMutable(
	const QString &strDeviceId)
{
	KTrustedDevice *pPending = nullptr;
	for (KTrustedDevice &device : m_devices)
	{
		if (device.strDeviceId != strDeviceId)
			continue;
		if (device.commitState == MutualTrustedDeviceCommitState)
			return &device;
		pPending = &device;
	}
	return pPending;
}

KTrustedDevice *KTrustedDeviceService::findTransactionMutable(
	const QString &strRequestId)
{
	for (KTrustedDevice &device : m_devices)
	{
		if (device.strPairingTransactionId == strRequestId)
			return &device;
	}
	return nullptr;
}

void KTrustedDeviceService::clearTransaction()
{
	m_beforeTransaction.clear();
	m_strTransactionId.clear();
	m_strTransactionDeviceId.clear();
}

bool KTrustedDeviceService::saveCurrent(QString *pErrorMessage)
{
	return m_pStore->saveDevices(m_devices, pErrorMessage);
}
