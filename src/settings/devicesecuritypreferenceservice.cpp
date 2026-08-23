#include "settings/devicesecuritypreferenceservice.h"

#include "common/sessiontracelogger.h"
#include "core/settings/devicesecuritypreferencestore.h"
#include "session/sessioncontroller.h"

#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <algorithm>

namespace
{
	constexpr int kMaximumPreferences = 128;

	bool IsValidDeviceId(const QString &strDeviceId)
	{
		return !strDeviceId.isEmpty() && !QUuid(strDeviceId).isNull();
	}

	QString ShortDeviceId(const QString &strDeviceId)
	{
		return strDeviceId.left(12);
	}
}

KDeviceSecurityPreferenceService::KDeviceSecurityPreferenceService(
	std::unique_ptr<KDeviceSecurityPreferenceStore> spStore,
	KSessionController *pSessionController,
	QObject *pParent)
	: QObject(pParent)
	, m_spStore(std::move(spStore))
	, m_pSessionController(pSessionController)
{
	Q_ASSERT(m_spStore != nullptr);
	Q_ASSERT(m_pSessionController != nullptr);
	connect(m_pSessionController, &KSessionController::deviceAuthenticationStateChanged,
		this,
		[this](const QString &strState, const QString &strDeviceId,
			const QString &, bool)
		{
			handleDeviceAuthenticationStateChanged(strState, strDeviceId);
		});
	connect(m_pSessionController, &KSessionController::sessionCapabilitiesChanged,
		this, &KDeviceSecurityPreferenceService::handleSessionCapabilitiesChanged);
	connect(m_pSessionController, &KSessionController::sessionStateChanged,
		this, &KDeviceSecurityPreferenceService::handleSessionStateChanged);
	connect(m_pSessionController, &KSessionController::privacyModeStatusChanged,
		this, &KDeviceSecurityPreferenceService::handlePrivacyModeStatusChanged);
	connect(m_pSessionController, &KSessionController::postSessionActionStatusChanged,
		this, &KDeviceSecurityPreferenceService::handlePostSessionActionStatusChanged);
	connect(m_pSessionController, &KSessionController::privacyModeCommandCompleted,
		this, &KDeviceSecurityPreferenceService::handlePrivacyModeCommandCompleted);
	connect(m_pSessionController, &KSessionController::postSessionActionCommandCompleted,
		this, &KDeviceSecurityPreferenceService::handlePostSessionActionCommandCompleted);
}

KDeviceSecurityPreferenceService::~KDeviceSecurityPreferenceService()
{
}

void KDeviceSecurityPreferenceService::initialize()
{
	QString strError;
	const QVector<KDeviceSecurityPreference> preferences = m_spStore->load(&strError);
	for (const KDeviceSecurityPreference &preference : preferences)
	{
		if (!IsValidDeviceId(preference.strRemoteDeviceId))
			continue;
		const auto iterator = m_preferences.constFind(preference.strRemoteDeviceId);
		if (iterator == m_preferences.constEnd()
			|| iterator->nUpdatedAtMs < preference.nUpdatedAtMs)
		{
			m_preferences.insert(preference.strRemoteDeviceId, preference);
		}
	}
	const QVector<KDeviceSecurityPreference> retainedPreferences = preferenceList();
	m_preferences.clear();
	for (const KDeviceSecurityPreference &preference : retainedPreferences)
		m_preferences.insert(preference.strRemoteDeviceId, preference);
	writeTrace(QStringLiteral("loaded"), QString(),
		QStringLiteral("count=%1").arg(m_preferences.size()));
	if (!strError.isEmpty())
	{
		writeTrace(QStringLiteral("load_failed"), QString(),
			QStringLiteral("error=%1").arg(strError));
		emit preferenceError(strError);
	}
}

KPrivacyModeStatus KDeviceSecurityPreferenceService::privacyModeStatus() const
{
	return m_privacyModeStatus;
}

KPostSessionActionStatus KDeviceSecurityPreferenceService::postSessionActionStatus() const
{
	return m_postSessionActionStatus;
}

bool KDeviceSecurityPreferenceService::isPrivacyCommandPending() const
{
	for (const KPendingPreferenceCommand &command : m_pendingCommands)
	{
		if (command.bPrivacyCommand)
			return true;
	}
	return false;
}

bool KDeviceSecurityPreferenceService::isPostSessionActionCommandPending() const
{
	for (const KPendingPreferenceCommand &command : m_pendingCommands)
	{
		if (!command.bPrivacyCommand)
			return true;
	}
	return false;
}

void KDeviceSecurityPreferenceService::requestPrivacyMode(KPrivacyMode mode)
{
	issuePrivacyMode(mode, true);
}

void KDeviceSecurityPreferenceService::requestPostSessionAction(
	KPostSessionAction action)
{
	issuePostSessionAction(action, true);
}

void KDeviceSecurityPreferenceService::removePreference(
	const QString &strRemoteDeviceId)
{
	if (!m_preferences.contains(strRemoteDeviceId))
		return;
	const QHash<QString, KDeviceSecurityPreference> oldPreferences = m_preferences;
	m_preferences.remove(strRemoteDeviceId);
	QString strError;
	const QVector<KDeviceSecurityPreference> storedPreferences = preferenceList();
	if (m_spStore->save(storedPreferences, &strError))
	{
		writeTrace(QStringLiteral("removed"), strRemoteDeviceId, QString());
		return;
	}
	m_preferences = oldPreferences;
	writeTrace(QStringLiteral("save_failed"), strRemoteDeviceId,
		QStringLiteral("error=%1").arg(strError));
	emit preferenceError(strError);
}

void KDeviceSecurityPreferenceService::handleDeviceAuthenticationStateChanged(
	const QString &strState,
	const QString &strDeviceId)
{
	synchronizeGeneration();
	if (m_pSessionController->sessionRole() != ControllerSessionRole
		|| strState != QStringLiteral("authenticated")
		|| !IsValidDeviceId(strDeviceId))
	{
		return;
	}
	m_strRemoteDeviceId = strDeviceId;
	const bool bFound = m_preferences.contains(strDeviceId);
	writeTrace(QStringLiteral("selected"), strDeviceId,
		QStringLiteral("found=%1 generation=%2").arg(bFound ? 1 : 0).arg(m_nGeneration));
	tryApplyRememberedPreferences();
}

void KDeviceSecurityPreferenceService::handleSessionCapabilitiesChanged(
	const KNegotiatedCapabilities &capabilities)
{
	synchronizeGeneration();
	m_capabilities = capabilities;
	tryApplyRememberedPreferences();
}

void KDeviceSecurityPreferenceService::handleSessionStateChanged(KSessionState state)
{
	synchronizeGeneration();
	m_sessionState = state;
	if (state == IdleSessionState || state == ListeningSessionState
		|| state == StoppingSessionState || state == ShutdownTimedOutSessionState)
	{
		clearSessionState(m_pSessionController->sessionGeneration());
		m_sessionState = state;
		return;
	}
	tryApplyRememberedPreferences();
}

void KDeviceSecurityPreferenceService::handlePrivacyModeStatusChanged(
	const KPrivacyModeStatus &status)
{
	synchronizeGeneration();
	if (status.nGeneration == m_nGeneration)
		m_privacyModeStatus = status;
}

void KDeviceSecurityPreferenceService::handlePostSessionActionStatusChanged(
	const KPostSessionActionStatus &status)
{
	synchronizeGeneration();
	if (status.nGeneration == m_nGeneration)
		m_postSessionActionStatus = status;
}

void KDeviceSecurityPreferenceService::handlePrivacyModeCommandCompleted(
	const QString &strRequestId,
	bool bSuccess,
	const QString &strErrorCode)
{
	completeCommand(strRequestId, bSuccess, strErrorCode, true);
}

void KDeviceSecurityPreferenceService::handlePostSessionActionCommandCompleted(
	const QString &strRequestId,
	bool bSuccess,
	const QString &strErrorCode)
{
	completeCommand(strRequestId, bSuccess, strErrorCode, false);
}

void KDeviceSecurityPreferenceService::synchronizeGeneration()
{
	const quint64 nGeneration = m_pSessionController->sessionGeneration();
	if (nGeneration != m_nGeneration)
		clearSessionState(nGeneration);
}

void KDeviceSecurityPreferenceService::clearSessionState(quint64 nGeneration)
{
	m_strRemoteDeviceId.clear();
	m_nGeneration = nGeneration;
	m_capabilities = KNegotiatedCapabilities();
	m_pendingCommands.clear();
	m_bPrivacyAutoApplyAttempted = false;
	m_bPostActionAutoApplyAttempted = false;
	m_privacyModeStatus = KPrivacyModeStatus();
	m_privacyModeStatus.nGeneration = nGeneration;
	m_postSessionActionStatus = KPostSessionActionStatus();
	m_postSessionActionStatus.nGeneration = nGeneration;
}

void KDeviceSecurityPreferenceService::tryApplyRememberedPreferences()
{
	if (m_pSessionController->sessionRole() != ControllerSessionRole
		|| m_sessionState != StreamingSessionState
		|| !IsValidDeviceId(m_strRemoteDeviceId))
	{
		return;
	}
	const auto iterator = m_preferences.constFind(m_strRemoteDeviceId);
	if (iterator == m_preferences.constEnd())
	{
		m_bPrivacyAutoApplyAttempted = true;
		m_bPostActionAutoApplyAttempted = true;
		return;
	}
	const KDeviceSecurityPreference preference = iterator.value();
	if (!m_bPrivacyAutoApplyAttempted)
	{
		m_bPrivacyAutoApplyAttempted = true;
		const QString strMode = DeviceSecurityPrivacyModeName(
			preference.desiredPrivacyMode);
		if (preference.desiredPrivacyMode == DisabledPrivacyMode)
		{
			writeTrace(QStringLiteral("auto_apply_skipped"), m_strRemoteDeviceId,
				QStringLiteral("type=privacy reason=default_disabled generation=%1")
					.arg(m_nGeneration));
		}
		else if (!m_capabilities.supportedPrivacyModes.contains(strMode))
		{
			writeTrace(QStringLiteral("auto_apply_skipped"), m_strRemoteDeviceId,
				QStringLiteral("type=privacy mode=%1 reason=unsupported generation=%2")
					.arg(strMode).arg(m_nGeneration));
		}
		else
		{
			issuePrivacyMode(preference.desiredPrivacyMode, false);
		}
	}

	if (!m_bPostActionAutoApplyAttempted)
	{
		m_bPostActionAutoApplyAttempted = true;
		if (preference.desiredPostSessionAction == NoPostSessionAction)
		{
			writeTrace(QStringLiteral("auto_apply_skipped"), m_strRemoteDeviceId,
				QStringLiteral("type=post_action reason=default_none generation=%1")
					.arg(m_nGeneration));
		}
		else if (!m_capabilities.bPostSessionLock)
		{
			writeTrace(QStringLiteral("auto_apply_skipped"), m_strRemoteDeviceId,
				QStringLiteral("type=post_action action=lockworkstation "
					"reason=unsupported generation=%1").arg(m_nGeneration));
		}
		else
		{
			issuePostSessionAction(preference.desiredPostSessionAction, false);
		}
	}
}

QString KDeviceSecurityPreferenceService::issuePrivacyMode(KPrivacyMode mode,
	bool bUserInitiated)
{
	synchronizeGeneration();
	const QString strRequestId = m_pSessionController->requestPrivacyMode(mode);
	if (strRequestId.isEmpty())
		return QString();
	KPendingPreferenceCommand command;
	command.strRemoteDeviceId = m_strRemoteDeviceId;
	command.nGeneration = m_nGeneration;
	command.privacyMode = mode;
	command.bPrivacyCommand = true;
	command.bUserInitiated = bUserInitiated;
	m_pendingCommands.insert(strRequestId, command);
	emit privacyModeCommandStarted();
	if (!bUserInitiated)
	{
		writeTrace(QStringLiteral("auto_apply"), m_strRemoteDeviceId,
			QStringLiteral("type=privacy mode=%1 generation=%2 requestId=%3")
				.arg(DeviceSecurityPrivacyModeName(mode)).arg(m_nGeneration).arg(strRequestId));
	}
	return strRequestId;
}

QString KDeviceSecurityPreferenceService::issuePostSessionAction(
	KPostSessionAction action,
	bool bUserInitiated)
{
	synchronizeGeneration();
	const QString strRequestId = m_pSessionController->requestPostSessionAction(action);
	if (strRequestId.isEmpty())
		return QString();
	KPendingPreferenceCommand command;
	command.strRemoteDeviceId = m_strRemoteDeviceId;
	command.nGeneration = m_nGeneration;
	command.postSessionAction = action;
	command.bPrivacyCommand = false;
	command.bUserInitiated = bUserInitiated;
	m_pendingCommands.insert(strRequestId, command);
	emit postSessionActionCommandStarted();
	if (!bUserInitiated)
	{
		writeTrace(QStringLiteral("auto_apply"), m_strRemoteDeviceId,
			QStringLiteral("type=post_action action=%1 generation=%2 requestId=%3")
				.arg(DeviceSecurityPostSessionActionName(action))
				.arg(m_nGeneration).arg(strRequestId));
	}
	return strRequestId;
}

void KDeviceSecurityPreferenceService::completeCommand(const QString &strRequestId,
	bool bSuccess,
	const QString &strErrorCode,
	bool bPrivacyCommand)
{
	const auto iterator = m_pendingCommands.find(strRequestId);
	if (iterator == m_pendingCommands.end())
		return;
	const KPendingPreferenceCommand command = iterator.value();
	m_pendingCommands.erase(iterator);
	if (command.bPrivacyCommand != bPrivacyCommand
		|| command.nGeneration != m_nGeneration
		|| command.strRemoteDeviceId.isEmpty()
		|| command.strRemoteDeviceId != m_strRemoteDeviceId)
	{
		return;
	}
	if (!bSuccess)
	{
		if (!command.bUserInitiated)
		{
			writeTrace(QStringLiteral("auto_apply_failed"), command.strRemoteDeviceId,
				QStringLiteral("generation=%1 requestId=%2 errorCode=%3")
					.arg(command.nGeneration).arg(strRequestId, strErrorCode));
		}
		return;
	}
	if (command.bUserInitiated)
		updatePreference(command);
}

bool KDeviceSecurityPreferenceService::updatePreference(
	const KPendingPreferenceCommand &command)
{
	const QHash<QString, KDeviceSecurityPreference> oldPreferences = m_preferences;
	KDeviceSecurityPreference preference = m_preferences.value(command.strRemoteDeviceId);
	preference.strRemoteDeviceId = command.strRemoteDeviceId;
	if (command.bPrivacyCommand)
		preference.desiredPrivacyMode = command.privacyMode;
	else
		preference.desiredPostSessionAction = command.postSessionAction;
	preference.nUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
	m_preferences.insert(command.strRemoteDeviceId, preference);
	QString strError;
	const QVector<KDeviceSecurityPreference> storedPreferences = preferenceList();
	if (m_spStore->save(storedPreferences, &strError))
	{
		m_preferences.clear();
		for (const KDeviceSecurityPreference &storedPreference : storedPreferences)
			m_preferences.insert(storedPreference.strRemoteDeviceId, storedPreference);
		writeTrace(QStringLiteral("saved"), command.strRemoteDeviceId,
			QStringLiteral("generation=%1 privacy=%2 postAction=%3")
				.arg(command.nGeneration)
				.arg(DeviceSecurityPrivacyModeName(preference.desiredPrivacyMode),
					DeviceSecurityPostSessionActionName(
						preference.desiredPostSessionAction)));
		return true;
	}
	m_preferences = oldPreferences;
	writeTrace(QStringLiteral("save_failed"), command.strRemoteDeviceId,
		QStringLiteral("generation=%1 error=%2").arg(command.nGeneration).arg(strError));
	emit preferenceError(strError);
	return false;
}

QVector<KDeviceSecurityPreference>
KDeviceSecurityPreferenceService::preferenceList() const
{
	QVector<KDeviceSecurityPreference> preferences = m_preferences.values();
	std::sort(preferences.begin(), preferences.end(),
		[](const KDeviceSecurityPreference &left,
			const KDeviceSecurityPreference &right)
		{
			return left.nUpdatedAtMs > right.nUpdatedAtMs;
		});
	if (preferences.size() > kMaximumPreferences)
		preferences.resize(kMaximumPreferences);
	return preferences;
}

void KDeviceSecurityPreferenceService::writeTrace(const QString &strStage,
	const QString &strDeviceId,
	const QString &strExtra) const
{
	QString strDetails = strExtra;
	if (!strDeviceId.isEmpty())
	{
		if (!strDetails.isEmpty())
			strDetails.prepend(QLatin1Char(' '));
		strDetails.prepend(QStringLiteral("deviceId=%1").arg(ShortDeviceId(strDeviceId)));
	}
	KSessionTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("device_preference_%1").arg(strStage),
		QStringLiteral("handled"), -1, strDetails);
}
