#include "privacy/privacymodeservice.h"

#include <QtCore/QMetaObject>

KPrivacyModeService::KPrivacyModeService(
	std::unique_ptr<IKPrivacyOverlayAdapter> spOverlayAdapter,
	std::unique_ptr<IKDisplayPowerAdapter> spDisplayPowerAdapter,
	QObject *pParent)
	: QObject(pParent)
	, m_spOverlayAdapter(std::move(spOverlayAdapter))
	, m_spDisplayPowerAdapter(std::move(spDisplayPowerAdapter))
{
	Q_ASSERT(m_spOverlayAdapter != nullptr);
	Q_ASSERT(m_spDisplayPowerAdapter != nullptr);
	m_spOverlayAdapter->setEmergencyRestoreHandler(
		[this]()
		{
			QMetaObject::invokeMethod(this,
				[this]() { handleEmergencyRestore(); }, Qt::QueuedConnection);
		});
}

KPrivacyModeService::~KPrivacyModeService()
{
	m_spOverlayAdapter->setEmergencyRestoreHandler({});
	m_spOverlayAdapter->restore();
	m_spDisplayPowerAdapter->restore();
}

void KPrivacyModeService::beginSession(quint64 nGeneration)
{
	m_spOverlayAdapter->restore();
	m_spDisplayPowerAdapter->restore();
	m_status = KPrivacyModeStatus();
	m_status.nGeneration = nGeneration;
	publishStatus();
}

QStringList KPrivacyModeService::supportedModes(bool bAdvertiseDisplayOff) const
{
	QStringList modes { QStringLiteral("disabled") };
	if (m_spOverlayAdapter->isSupported())
		modes.append(QStringLiteral("privacyoverlay"));
	if (bAdvertiseDisplayOff && m_spDisplayPowerAdapter->isSupported())
		modes.append(QStringLiteral("displayoff"));
	return modes;
}

KPrivacyOperationResult KPrivacyModeService::setMode(KPrivacyMode mode,
	const QString &strRequestId,
	quint64 nGeneration)
{
	if (nGeneration == 0 || nGeneration != m_status.nGeneration)
	{
		return KPrivacyOperationResult::failure(QStringLiteral("stale_generation"),
			QStringLiteral("Privacy mode command belongs to another session"));
	}
	if (mode == UnknownPrivacyMode
		|| (mode == PrivacyOverlayPrivacyMode && !m_spOverlayAdapter->isSupported())
		|| (mode == DisplayOffPrivacyMode && !m_spDisplayPowerAdapter->isSupported()))
	{
		return KPrivacyOperationResult::failure(QStringLiteral("unsupported_mode"),
			QStringLiteral("Privacy mode is unavailable on this host"));
	}

	m_status.strRequestId = strRequestId;
	m_status.strErrorCode.clear();
	m_status.requestedMode = mode;
	if (mode == m_status.effectiveMode
		&& (m_status.state == ActivePrivacyModeState
			|| m_status.state == InactivePrivacyModeState))
	{
		publishStatus();
		return KPrivacyOperationResult::success();
	}

	m_status.state = ApplyingPrivacyModeState;
	publishStatus();
	const KPrivacyMode oldMode = m_status.effectiveMode;
	const KPrivacyOperationResult restoreResult = restoreMode(oldMode);
	if (!restoreResult.bSucceeded)
	{
		m_status.state = FailedPrivacyModeState;
		m_status.strErrorCode = restoreResult.strErrorCode;
		publishStatus();
		return restoreResult;
	}

	m_status.effectiveMode = DisabledPrivacyMode;
	if (mode == DisabledPrivacyMode)
	{
		m_status.state = InactivePrivacyModeState;
		publishStatus();
		return KPrivacyOperationResult::success();
	}

	const KPrivacyOperationResult applyResult = applyMode(mode);
	if (!applyResult.bSucceeded)
	{
		restoreMode(mode);
		m_status.state = FailedPrivacyModeState;
		m_status.strErrorCode = applyResult.strErrorCode;
		publishStatus();
		return applyResult;
	}
	m_status.effectiveMode = mode;
	m_status.state = ActivePrivacyModeState;
	publishStatus();
	return KPrivacyOperationResult::success();
}

KPrivacyOperationResult KPrivacyModeService::reset(quint64 nGeneration)
{
	if (nGeneration != 0 && nGeneration != m_status.nGeneration)
	{
		return KPrivacyOperationResult::failure(QStringLiteral("stale_generation"),
			QStringLiteral("Privacy reset belongs to another session"));
	}
	m_status.requestedMode = DisabledPrivacyMode;
	m_status.state = RestoringPrivacyModeState;
	publishStatus();
	const KPrivacyOperationResult overlayResult = m_spOverlayAdapter->restore();
	const KPrivacyOperationResult displayResult = m_spDisplayPowerAdapter->restore();
	m_status.effectiveMode = DisabledPrivacyMode;
	if (!overlayResult.bSucceeded || !displayResult.bSucceeded)
	{
		const KPrivacyOperationResult result = !overlayResult.bSucceeded
			? overlayResult : displayResult;
		m_status.state = FailedPrivacyModeState;
		m_status.strErrorCode = result.strErrorCode.isEmpty()
			? QStringLiteral("restore_failed") : result.strErrorCode;
		publishStatus();
		return result;
	}
	m_status.state = InactivePrivacyModeState;
	m_status.strErrorCode.clear();
	publishStatus();
	return KPrivacyOperationResult::success();
}

KPrivacyModeStatus KPrivacyModeService::status() const
{
	return m_status;
}

KPrivacyOperationResult KPrivacyModeService::applyMode(KPrivacyMode mode)
{
	if (mode == PrivacyOverlayPrivacyMode)
		return m_spOverlayAdapter->apply();
	if (mode == DisplayOffPrivacyMode)
		return m_spDisplayPowerAdapter->turnOff();
	return KPrivacyOperationResult::success();
}

KPrivacyOperationResult KPrivacyModeService::restoreMode(KPrivacyMode mode)
{
	if (mode == PrivacyOverlayPrivacyMode)
		return m_spOverlayAdapter->restore();
	if (mode == DisplayOffPrivacyMode)
		return m_spDisplayPowerAdapter->restore();
	return KPrivacyOperationResult::success();
}

void KPrivacyModeService::publishStatus()
{
	emit statusChanged(m_status);
}

void KPrivacyModeService::handleEmergencyRestore()
{
	if (m_status.effectiveMode != PrivacyOverlayPrivacyMode)
		return;
	const quint64 nGeneration = m_status.nGeneration;
	m_status.strRequestId.clear();
	reset(nGeneration);
	emit emergencyRestoreTriggered(nGeneration);
}
