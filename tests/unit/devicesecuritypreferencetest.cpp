#include "adapters/settings/qsettingsdevicesecuritypreferencestore.h"
#include "core/settings/devicesecuritypreferencestore.h"
#include "session/sessioncontroller.h"
#include "settings/devicesecuritypreferenceservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>

#include <cstdio>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::fprintf(stderr, "FAILED: %s\n", strDescription.toUtf8().constData());
		++g_nFailureCount;
	}

	QString DeviceId(int nSuffix)
	{
		return QStringLiteral("00000000-0000-0000-0000-%1")
			.arg(nSuffix, 12, 10, QLatin1Char('0'));
	}

	class KFakeDeviceSecurityPreferenceStore final
		: public KDeviceSecurityPreferenceStore
	{
	public:
		QVector<KDeviceSecurityPreference> load(QString *pError) override
		{
			if (pError != nullptr)
				*pError = strLoadError;
			return preferences;
		}

		bool save(const QVector<KDeviceSecurityPreference> &newPreferences,
			QString *pError) override
		{
			++nSaveCount;
			if (!bSaveSucceeds)
			{
				if (pError != nullptr)
					*pError = QStringLiteral("simulated save failure");
				return false;
			}
			preferences = newPreferences;
			return true;
		}

		QVector<KDeviceSecurityPreference> preferences;
		QString strLoadError;
		int nSaveCount = 0;
		bool bSaveSucceeds = true;
	};

	class KFakeSessionController final : public KSessionController
	{
	public:
		void setRole(const QString &) override {}
		void startSignalingServer(quint16) override {}
		void connectSignaling(const QString &, quint16) override {}
		void retryLastConnection() override {}
		void disconnectSession() override {}
		void enterRemoteDesktop(const KStreamConfig &) override {}
		void leaveRemoteDesktop() override {}
		void startStreaming() override {}
		void stopStreaming() override {}
		void pushVideoFrame(const KVideoFrame &) override {}
		void sendInputMessage(const KInputMessage &) override {}
		void sendClipboardMessage(const KClipboardMessage &) override {}
		bool sendTerminalControlMessage(const KTerminalMessage &) override { return false; }
		bool sendTerminalData(const QByteArray &) override { return false; }
		bool isTerminalBackpressured() const override { return false; }
		void sendStreamConfig(const KStreamConfig &) override {}
		QString requestPrivacyMode(KPrivacyMode mode) override
		{
			if (bRejectRequests)
				return QString();
			privacyRequests.append(mode);
			const QString strRequestId = QStringLiteral("privacy-%1").arg(++nRequestSequence);
			requestTypes.insert(strRequestId, true);
			return strRequestId;
		}
		QString requestPostSessionAction(KPostSessionAction action) override
		{
			if (bRejectRequests)
				return QString();
			postActionRequests.append(action);
			const QString strRequestId = QStringLiteral("post-%1").arg(++nRequestSequence);
			requestTypes.insert(strRequestId, false);
			return strRequestId;
		}
		void handleCaptureFailure() override {}
		void applyApplicationSettings(const KApplicationSettings &) override {}
		void respondIncomingAccessRequest(const QString &, bool) override {}
		void respondPairingRequest(const QString &, bool, KPermissionScopes) override {}
		quint64 sessionGeneration() const override { return nGeneration; }
		KSessionRole sessionRole() const override { return role; }
		bool isIdle() const override { return state == IdleSessionState; }
		bool matchesCurrentEndpoint(const QString &, quint16) const override { return false; }

		void authenticate(const QString &strDeviceId)
		{
			emit deviceAuthenticationStateChanged(QStringLiteral("authenticated"),
				strDeviceId, QString(), true);
		}

		void publishCapabilities(bool bPrivacy, bool bPostSessionLock)
		{
			KNegotiatedCapabilities capabilities;
			capabilities.bValid = true;
			capabilities.supportedPrivacyModes.append(QStringLiteral("disabled"));
			if (bPrivacy)
				capabilities.supportedPrivacyModes.append(QStringLiteral("privacyoverlay"));
			capabilities.bPostSessionLock = bPostSessionLock;
			emit sessionCapabilitiesChanged(capabilities);
		}

		void publishState(KSessionState newState)
		{
			state = newState;
			emit sessionStateChanged(newState);
		}

		void completeLastPrivacy(bool bSuccess, const QString &strError = QString())
		{
			for (auto iterator = requestTypes.constEnd(); iterator != requestTypes.constBegin();)
			{
				--iterator;
				if (iterator.value())
				{
					emit privacyModeCommandCompleted(iterator.key(), bSuccess, strError);
					return;
				}
			}
		}

		void completeLastPostAction(bool bSuccess, const QString &strError = QString())
		{
			for (auto iterator = requestTypes.constEnd(); iterator != requestTypes.constBegin();)
			{
				--iterator;
				if (!iterator.value())
				{
					emit postSessionActionCommandCompleted(iterator.key(), bSuccess, strError);
					return;
				}
			}
		}

		KSessionRole role = ControllerSessionRole;
		KSessionState state = IdleSessionState;
		quint64 nGeneration = 1;
		int nRequestSequence = 0;
		bool bRejectRequests = false;
		QVector<KPrivacyMode> privacyRequests;
		QVector<KPostSessionAction> postActionRequests;
		QMap<QString, bool> requestTypes;
	};

	KDeviceSecurityPreference Preference(const QString &strDeviceId,
		KPrivacyMode mode,
		KPostSessionAction action,
		qint64 nUpdatedAtMs)
	{
		KDeviceSecurityPreference preference;
		preference.strRemoteDeviceId = strDeviceId;
		preference.desiredPrivacyMode = mode;
		preference.desiredPostSessionAction = action;
		preference.nUpdatedAtMs = nUpdatedAtMs;
		return preference;
	}

	void TestQSettingsRoundTripAndSanitization()
	{
		QTemporaryDir temporaryDir;
		const QString strFilePath = temporaryDir.filePath(
			QStringLiteral("device_security_preferences.ini"));
		KQSettingsDeviceSecurityPreferenceStore store(strFilePath);
		QVector<KDeviceSecurityPreference> source;
		for (int nIndex = 1; nIndex <= 130; ++nIndex)
		{
			source.append(Preference(DeviceId(nIndex),
				nIndex == 130 ? PrivacyOverlayPrivacyMode : DisabledPrivacyMode,
				nIndex == 130 ? LockWorkstationPostSessionAction : NoPostSessionAction,
				nIndex));
		}
		QString strError;
		const bool bSaved = store.save(source, &strError);
		Check(bSaved,
			QStringLiteral("preference store saves values: %1").arg(strError));
		const QVector<KDeviceSecurityPreference> loaded = store.load(&strError);
		Check(loaded.size() == 128
			&& loaded.first().strRemoteDeviceId == DeviceId(130)
			&& loaded.first().desiredPrivacyMode == PrivacyOverlayPrivacyMode
			&& loaded.first().desiredPostSessionAction == LockWorkstationPostSessionAction,
			QStringLiteral("preference store sorts, caps and round-trips values"));

		QSettings rawSettings(strFilePath, QSettings::IniFormat);
		rawSettings.beginWriteArray(QStringLiteral("deviceSecurityPreferences"), 1);
		rawSettings.setArrayIndex(0);
		rawSettings.setValue(QStringLiteral("deviceId"), DeviceId(200));
		rawSettings.setValue(QStringLiteral("privacyMode"), QStringLiteral("privacyoverlay"));
		rawSettings.setValue(QStringLiteral("postSessionAction"),
			QStringLiteral("lockworkstation"));
		rawSettings.setValue(QStringLiteral("updatedAtMs"), -1);
		rawSettings.endArray();
		rawSettings.sync();
		const QVector<KDeviceSecurityPreference> sanitized = store.load(&strError);
		Check(sanitized.size() == 1
			&& sanitized.first().desiredPrivacyMode == DisabledPrivacyMode
			&& sanitized.first().desiredPostSessionAction == NoPostSessionAction
			&& sanitized.first().nUpdatedAtMs == 0,
			QStringLiteral("invalid stored fields use safe defaults"));
	}

	void TestAutoApplyAndUserPersistence()
	{
		auto spStore = std::make_unique<KFakeDeviceSecurityPreferenceStore>();
		KFakeDeviceSecurityPreferenceStore *pStore = spStore.get();
		const QString strDeviceId = DeviceId(1);
		pStore->preferences.append(Preference(strDeviceId,
			PrivacyOverlayPrivacyMode, LockWorkstationPostSessionAction, 1));
		KFakeSessionController controller;
		KDeviceSecurityPreferenceService service(std::move(spStore), &controller);
		service.initialize();

		controller.authenticate(strDeviceId);
		controller.publishCapabilities(true, true);
		controller.publishState(StreamingSessionState);
		Check(controller.privacyRequests == QVector<KPrivacyMode> { PrivacyOverlayPrivacyMode }
			&& controller.postActionRequests
				== QVector<KPostSessionAction> { LockWorkstationPostSessionAction },
			QStringLiteral("remembered settings apply after streaming"));
		controller.publishState(ReconnectingSessionState);
		controller.publishState(StreamingSessionState);
		Check(controller.privacyRequests.size() == 1
			&& controller.postActionRequests.size() == 1,
			QStringLiteral("recovery does not repeat automatic commands"));
		controller.completeLastPrivacy(true);
		controller.completeLastPostAction(true);
		Check(pStore->nSaveCount == 0,
			QStringLiteral("automatic application does not rewrite preferences"));

		controller.nGeneration = 2;
		controller.publishState(ConnectingSessionState);
		controller.authenticate(strDeviceId);
		controller.publishCapabilities(true, true);
		controller.publishState(StreamingSessionState);
		Check(controller.privacyRequests.size() == 2
			&& controller.privacyRequests.last() == PrivacyOverlayPrivacyMode
			&& controller.postActionRequests.size() == 2
			&& controller.postActionRequests.last()
				== LockWorkstationPostSessionAction,
			QStringLiteral("same device reapplies preferences in a new generation"));
		controller.completeLastPrivacy(true);
		controller.completeLastPostAction(true);
		Check(pStore->nSaveCount == 0,
			QStringLiteral("new-generation automatic application remains read-only"));

		service.requestPrivacyMode(DisabledPrivacyMode);
		controller.completeLastPrivacy(true);
		Check(pStore->nSaveCount == 1
			&& pStore->preferences.first().desiredPrivacyMode == DisabledPrivacyMode
			&& pStore->preferences.first().desiredPostSessionAction
				== LockWorkstationPostSessionAction,
			QStringLiteral("successful user command saves only its field"));
		pStore->bSaveSucceeds = false;
		service.requestPostSessionAction(NoPostSessionAction);
		controller.completeLastPostAction(true);
		Check(pStore->nSaveCount == 2
			&& pStore->preferences.first().desiredPostSessionAction
				== LockWorkstationPostSessionAction,
			QStringLiteral("store failure preserves previous preference"));
		pStore->bSaveSucceeds = true;
		service.requestPostSessionAction(NoPostSessionAction);
		controller.completeLastPostAction(false, QStringLiteral("permission_denied"));
		Check(pStore->nSaveCount == 2
			&& pStore->preferences.first().desiredPostSessionAction
				== LockWorkstationPostSessionAction,
			QStringLiteral("failed user command preserves previous preference"));
	}

	void TestDeviceGenerationCapabilityAndRevocationBoundaries()
	{
		auto spStore = std::make_unique<KFakeDeviceSecurityPreferenceStore>();
		KFakeDeviceSecurityPreferenceStore *pStore = spStore.get();
		const QString strFirstDevice = DeviceId(10);
		const QString strSecondDevice = DeviceId(11);
		pStore->preferences = {
			Preference(strFirstDevice, PrivacyOverlayPrivacyMode,
				LockWorkstationPostSessionAction, 2),
			Preference(strSecondDevice, DisabledPrivacyMode,
				NoPostSessionAction, 1)
		};
		KFakeSessionController controller;
		KDeviceSecurityPreferenceService service(std::move(spStore), &controller);
		service.initialize();

		controller.authenticate(strFirstDevice);
		controller.publishCapabilities(false, false);
		controller.publishState(StreamingSessionState);
		Check(controller.privacyRequests.isEmpty()
			&& controller.postActionRequests.isEmpty(),
			QStringLiteral("unsupported remembered settings are skipped"));
		Check(pStore->preferences.size() == 2,
			QStringLiteral("unsupported settings remain persisted"));

		controller.nGeneration = 2;
		controller.publishState(ConnectingSessionState);
		controller.authenticate(strSecondDevice);
		controller.publishCapabilities(true, true);
		controller.publishState(StreamingSessionState);
		service.requestPrivacyMode(PrivacyOverlayPrivacyMode);
		controller.nGeneration = 3;
		controller.publishState(ConnectingSessionState);
		controller.completeLastPrivacy(true);
		Check(pStore->nSaveCount == 0,
			QStringLiteral("old generation completion cannot save a preference"));

		service.removePreference(strFirstDevice);
		Check(pStore->nSaveCount == 1 && pStore->preferences.size() == 1
			&& pStore->preferences.first().strRemoteDeviceId == strSecondDevice,
			QStringLiteral("trusted-device revocation removes only matching preference"));

		controller.role = ControlledSessionRole;
		controller.nGeneration = 4;
		controller.publishState(ConnectingSessionState);
		controller.authenticate(strSecondDevice);
		controller.publishCapabilities(true, true);
		controller.publishState(StreamingSessionState);
		Check(controller.privacyRequests.size() == 1,
			QStringLiteral("controlled role does not auto-apply controller preferences"));
	}

	void TestCachedRuntimeStatus()
	{
		auto spStore = std::make_unique<KFakeDeviceSecurityPreferenceStore>();
		KFakeSessionController controller;
		KDeviceSecurityPreferenceService service(std::move(spStore), &controller);
		service.initialize();
		KPrivacyModeStatus privacyStatus;
		privacyStatus.nGeneration = 1;
		privacyStatus.effectiveMode = PrivacyOverlayPrivacyMode;
		privacyStatus.state = ActivePrivacyModeState;
		emit controller.privacyModeStatusChanged(privacyStatus);
		KPostSessionActionStatus postStatus;
		postStatus.nGeneration = 1;
		postStatus.action = LockWorkstationPostSessionAction;
		emit controller.postSessionActionStatusChanged(postStatus);
		Check(service.privacyModeStatus().effectiveMode == PrivacyOverlayPrivacyMode
			&& service.postSessionActionStatus().action
				== LockWorkstationPostSessionAction,
			QStringLiteral("service caches actual runtime status for late windows"));
	}

	void TestServiceReconstructionAndDeviceIsolation()
	{
		QTemporaryDir temporaryDir;
		const QString strFilePath = temporaryDir.filePath(
			QStringLiteral("device_security_preferences.ini"));
		const QString strFirstDevice = DeviceId(50);
		const QString strSecondDevice = DeviceId(51);
		{
			KFakeSessionController controller;
			auto spStore = std::make_unique<KQSettingsDeviceSecurityPreferenceStore>(
				strFilePath);
			KDeviceSecurityPreferenceService service(std::move(spStore), &controller);
			service.initialize();
			controller.authenticate(strFirstDevice);
			controller.publishCapabilities(true, true);
			controller.publishState(StreamingSessionState);
			service.requestPrivacyMode(PrivacyOverlayPrivacyMode);
			controller.completeLastPrivacy(true);
			service.requestPostSessionAction(LockWorkstationPostSessionAction);
			controller.completeLastPostAction(true);

			controller.nGeneration = 2;
			controller.publishState(ConnectingSessionState);
			controller.authenticate(strSecondDevice);
			controller.publishCapabilities(true, true);
			controller.publishState(StreamingSessionState);
			service.requestPrivacyMode(DisabledPrivacyMode);
			controller.completeLastPrivacy(true);
			service.requestPostSessionAction(NoPostSessionAction);
			controller.completeLastPostAction(true);
		}
		{
			KFakeSessionController controller;
			auto spStore = std::make_unique<KQSettingsDeviceSecurityPreferenceStore>(
				strFilePath);
			KDeviceSecurityPreferenceService service(std::move(spStore), &controller);
			service.initialize();
			controller.authenticate(strFirstDevice);
			controller.publishCapabilities(true, true);
			controller.publishState(StreamingSessionState);
			Check(controller.privacyRequests
				== QVector<KPrivacyMode>{PrivacyOverlayPrivacyMode},
				QStringLiteral("process restart restores the first device preference"));
			Check(controller.postActionRequests
				== QVector<KPostSessionAction>{LockWorkstationPostSessionAction},
				QStringLiteral("process restart restores the first device post action"));

			controller.nGeneration = 2;
			controller.publishState(ConnectingSessionState);
			controller.authenticate(strSecondDevice);
			controller.publishCapabilities(true, true);
			controller.publishState(StreamingSessionState);
			Check(controller.privacyRequests.size() == 1
				&& controller.postActionRequests.size() == 1,
				QStringLiteral("second device keeps independent disabled and none preferences"));
		}
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestQSettingsRoundTripAndSanitization();
	TestAutoApplyAndUserPersistence();
	TestDeviceGenerationCapabilityAndRevocationBoundaries();
	TestCachedRuntimeStatus();
	TestServiceReconstructionAndDeviceIsolation();
	if (g_nFailureCount == 0)
		qInfo() << "All device security preference tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
