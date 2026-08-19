#include "privacy/postsessionactionservice.h"
#include "privacy/privacymodeservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>

#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const char *pDescription)
	{
		if (bCondition)
			return;
		std::cerr << "FAILED: " << pDescription << '\n';
		++g_nFailureCount;
	}

	class KFakeOverlayAdapter : public IKPrivacyOverlayAdapter
	{
	public:
		bool isSupported() const override { return bSupported; }
		KPrivacyOperationResult apply() override
		{
			++nApplyCount;
			return bApplySucceeds ? KPrivacyOperationResult::success()
				: KPrivacyOperationResult::failure(QStringLiteral("overlay_creation_failed"),
					QStringLiteral("fake apply failure"));
		}
		KPrivacyOperationResult restore() override
		{
			++nRestoreCount;
			return bRestoreSucceeds ? KPrivacyOperationResult::success()
				: KPrivacyOperationResult::failure(QStringLiteral("restore_failed"),
					QStringLiteral("fake restore failure"));
		}
		void setEmergencyRestoreHandler(EmergencyRestoreHandler handler) override
		{
			emergencyHandler = std::move(handler);
		}
		void triggerEmergency()
		{
			if (emergencyHandler)
				emergencyHandler();
		}

		bool bSupported = true;
		bool bApplySucceeds = true;
		bool bRestoreSucceeds = true;
		int nApplyCount = 0;
		int nRestoreCount = 0;
		EmergencyRestoreHandler emergencyHandler;
	};

	class KFakeDisplayPowerAdapter : public IKDisplayPowerAdapter
	{
	public:
		bool isSupported() const override { return bSupported; }
		KPrivacyOperationResult turnOff() override
		{
			++nTurnOffCount;
			return bTurnOffSucceeds ? KPrivacyOperationResult::success()
				: KPrivacyOperationResult::failure(QStringLiteral("display_power_failed"),
					QStringLiteral("fake display failure"));
		}
		KPrivacyOperationResult restore() override
		{
			++nRestoreCount;
			return KPrivacyOperationResult::success();
		}

		bool bSupported = true;
		bool bTurnOffSucceeds = true;
		int nTurnOffCount = 0;
		int nRestoreCount = 0;
	};

	class KFakeWorkstationLockAdapter : public IKWorkstationLockAdapter
	{
	public:
		bool isSupported() const override { return bSupported; }
		KPrivacyOperationResult lock() override
		{
			++nLockCount;
			return KPrivacyOperationResult::success();
		}

		bool bSupported = true;
		int nLockCount = 0;
	};

	void TestPrivacyModes()
	{
		auto spOverlay = std::make_unique<KFakeOverlayAdapter>();
		KFakeOverlayAdapter *pOverlay = spOverlay.get();
		auto spDisplay = std::make_unique<KFakeDisplayPowerAdapter>();
		KFakeDisplayPowerAdapter *pDisplay = spDisplay.get();
		KPrivacyModeService service(std::move(spOverlay), std::move(spDisplay));
		service.beginSession(7);
		Check(service.supportedModes(false)
			== QStringList({ QStringLiteral("disabled"),
				QStringLiteral("privacyoverlay") }),
			"display-off rollout remains hidden by default");
		Check(service.setMode(PrivacyOverlayPrivacyMode, QStringLiteral("one"), 7).bSucceeded
			&& service.status().effectiveMode == PrivacyOverlayPrivacyMode
			&& pOverlay->nApplyCount == 1,
			"privacy overlay becomes the effective mode");
		Check(service.setMode(PrivacyOverlayPrivacyMode, QStringLiteral("two"), 7).bSucceeded
			&& pOverlay->nApplyCount == 1,
			"repeating a privacy mode is idempotent");
		Check(service.setMode(DisplayOffPrivacyMode, QStringLiteral("three"), 7).bSucceeded
			&& pOverlay->nRestoreCount >= 2
			&& pDisplay->nTurnOffCount == 1,
			"switching modes restores the old adapter before applying the new one");
		Check(!service.setMode(PrivacyOverlayPrivacyMode, QStringLiteral("old"), 6).bSucceeded,
			"commands from an old generation are rejected");
	}

	void TestFailureAndEmergencyRestore()
	{
		auto spOverlay = std::make_unique<KFakeOverlayAdapter>();
		KFakeOverlayAdapter *pOverlay = spOverlay.get();
		auto spDisplay = std::make_unique<KFakeDisplayPowerAdapter>();
		KPrivacyModeService service(std::move(spOverlay), std::move(spDisplay));
		service.beginSession(8);
		pOverlay->bApplySucceeds = false;
		const KPrivacyOperationResult failure = service.setMode(
			PrivacyOverlayPrivacyMode, QStringLiteral("failed"), 8);
		Check(!failure.bSucceeded
			&& service.status().state == FailedPrivacyModeState
			&& service.status().effectiveMode == DisabledPrivacyMode,
			"failed apply reports the real disabled effective mode");

		pOverlay->bApplySucceeds = true;
		service.setMode(PrivacyOverlayPrivacyMode, QStringLiteral("active"), 8);
		pOverlay->triggerEmergency();
		QEventLoop wait;
		QTimer::singleShot(0, &wait, &QEventLoop::quit);
		wait.exec();
		Check(service.status().effectiveMode == DisabledPrivacyMode
			&& service.status().state == InactivePrivacyModeState,
			"local emergency restore disables the overlay");
	}

	void TestPostSessionAction()
	{
		auto spLock = std::make_unique<KFakeWorkstationLockAdapter>();
		KFakeWorkstationLockAdapter *pLock = spLock.get();
		KPostSessionActionService service(std::move(spLock));
		service.beginSession(10);
		service.setAction(LockWorkstationPostSessionAction, QStringLiteral("first"), 10);
		Check(service.consumeAfterTeardown(10).bSucceeded && pLock->nLockCount == 0,
			"a session that never streamed does not lock the workstation");

		service.beginSession(11);
		service.setAction(LockWorkstationPostSessionAction, QStringLiteral("second"), 11);
		service.markStreaming(11);
		Check(service.consumeAfterTeardown(11).bSucceeded && pLock->nLockCount == 1,
			"a streamed session locks once after teardown");
		Check(!service.consumeAfterTeardown(11).bSucceeded && pLock->nLockCount == 1,
			"repeated teardown cannot lock twice");
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestPrivacyModes();
	TestFailureAndEmergencyRestore();
	TestPostSessionAction();
	return g_nFailureCount == 0 ? 0 : 1;
}
