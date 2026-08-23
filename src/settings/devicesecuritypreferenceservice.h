#ifndef _WINREMOTECONTROL_SETTINGS_DEVICESECURITYPREFERENCESERVICE_H_
#define _WINREMOTECONTROL_SETTINGS_DEVICESECURITYPREFERENCESERVICE_H_

#include "core/protocol/sessionmessage.h"
#include "core/settings/devicesecuritypreference.h"
#include "core/session/sessionstatemachine.h"

#include <QtCore/QHash>
#include <QtCore/QObject>

#include <memory>

class KDeviceSecurityPreferenceStore;
class KSessionController;

class KDeviceSecurityPreferenceService final : public QObject
{
	Q_OBJECT

public:
	explicit KDeviceSecurityPreferenceService(
		std::unique_ptr<KDeviceSecurityPreferenceStore> spStore,
		KSessionController *pSessionController,
		QObject *pParent = nullptr);
	~KDeviceSecurityPreferenceService() override;

	KDeviceSecurityPreferenceService(const KDeviceSecurityPreferenceService &) = delete;
	KDeviceSecurityPreferenceService &operator=(
		const KDeviceSecurityPreferenceService &) = delete;

	void initialize();
	KPrivacyModeStatus privacyModeStatus() const;
	KPostSessionActionStatus postSessionActionStatus() const;
	bool isPrivacyCommandPending() const;
	bool isPostSessionActionCommandPending() const;

public slots:
	void requestPrivacyMode(KPrivacyMode mode);
	void requestPostSessionAction(KPostSessionAction action);
	void removePreference(const QString &strRemoteDeviceId);

signals:
	void privacyModeCommandStarted();
	void postSessionActionCommandStarted();
	void preferenceError(const QString &strError);

private:
	struct KPendingPreferenceCommand
	{
		QString strRemoteDeviceId;
		quint64 nGeneration = 0;
		KPrivacyMode privacyMode = DisabledPrivacyMode;
		KPostSessionAction postSessionAction = NoPostSessionAction;
		bool bPrivacyCommand = false;
		bool bUserInitiated = false;
	};

	void handleDeviceAuthenticationStateChanged(const QString &strState,
		const QString &strDeviceId);
	void handleSessionCapabilitiesChanged(const KNegotiatedCapabilities &capabilities);
	void handleSessionStateChanged(KSessionState state);
	void handlePrivacyModeStatusChanged(const KPrivacyModeStatus &status);
	void handlePostSessionActionStatusChanged(const KPostSessionActionStatus &status);
	void handlePrivacyModeCommandCompleted(const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode);
	void handlePostSessionActionCommandCompleted(const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode);
	void synchronizeGeneration();
	void clearSessionState(quint64 nGeneration);
	void tryApplyRememberedPreferences();
	QString issuePrivacyMode(KPrivacyMode mode, bool bUserInitiated);
	QString issuePostSessionAction(KPostSessionAction action, bool bUserInitiated);
	void completeCommand(const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode,
		bool bPrivacyCommand);
	bool updatePreference(const KPendingPreferenceCommand &command);
	QVector<KDeviceSecurityPreference> preferenceList() const;
	void writeTrace(const QString &strStage,
		const QString &strDeviceId,
		const QString &strExtra) const;

	std::unique_ptr<KDeviceSecurityPreferenceStore> m_spStore;
	KSessionController *m_pSessionController = nullptr;
	QHash<QString, KDeviceSecurityPreference> m_preferences;
	QHash<QString, KPendingPreferenceCommand> m_pendingCommands;
	QString m_strRemoteDeviceId;
	quint64 m_nGeneration = 0;
	KSessionState m_sessionState = IdleSessionState;
	KNegotiatedCapabilities m_capabilities;
	KPrivacyModeStatus m_privacyModeStatus;
	KPostSessionActionStatus m_postSessionActionStatus;
	bool m_bPrivacyAutoApplyAttempted = false;
	bool m_bPostActionAutoApplyAttempted = false;
};

#endif // _WINREMOTECONTROL_SETTINGS_DEVICESECURITYPREFERENCESERVICE_H_
