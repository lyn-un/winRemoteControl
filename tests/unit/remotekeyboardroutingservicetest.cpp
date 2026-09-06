#include "core/settings/keyboardsettingsstore.h"
#include "input/remotekeyboardroutingservice.h"
#include "settings/keyboardsettingsservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

class KMemoryKeyboardSettingsStore final : public KKeyboardSettingsStore
{
public:
	bool loadSettings(KKeyboardSettings *pSettings, QString *) override
	{
		if (pSettings != nullptr)
			*pSettings = m_settings;
		return true;
	}

	bool saveSettings(const KKeyboardSettings &settings, QString *) override
	{
		m_settings = settings;
		return true;
	}

private:
	KKeyboardSettings m_settings;
};

class KRemoteKeyboardRoutingServiceTestAccess
{
public:
	static void enableCapture(KRemoteKeyboardRoutingService *pService)
	{
		pService->m_mode = KRemoteKeyboardRoutingService::RemoteCaptureMode;
	}

	static bool route(KRemoteKeyboardRoutingService *pService,
		quint32 nVirtualKey,
		bool bPressed,
		quint32 nScanCode = 0,
		quint32 nFlags = 0)
	{
		KWindowsKeyboardEvent event;
		event.nVirtualKey = nVirtualKey;
		event.nScanCode = nScanCode;
		event.nFlags = nFlags;
		event.bPressed = bPressed;
		return pService->handleRemoteCaptureEvent(event);
	}

	static int pendingModifierCount(const KRemoteKeyboardRoutingService *pService)
	{
		return pService->m_pendingModifiers.size();
	}
};

namespace
{
	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	void testFunctionShortcutDoesNotReachRemote()
	{
		auto spStore = std::make_unique<KMemoryKeyboardSettingsStore>();
		KKeyboardSettingsService settingsService(std::move(spStore));
		settingsService.initialize();
		KRemoteKeyboardRoutingService service(&settingsService);
		KRemoteKeyboardRoutingServiceTestAccess::enableCapture(&service);
		int nRemoteEventCount = 0;
		int nFullscreenCount = 0;
		QObject::connect(&service, &KRemoteKeyboardRoutingService::remoteKeyRequested,
			[&nRemoteEventCount](int, int, bool, bool, bool) { ++nRemoteEventCount; });
		QObject::connect(&service, &KRemoteKeyboardRoutingService::toggleFullscreenRequested,
			[&nFullscreenCount]() { ++nFullscreenCount; });

		KRemoteKeyboardRoutingServiceTestAccess::route(&service, VK_LCONTROL, true, 0x1D);
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, VK_LMENU, true, 0x38);
		check(nRemoteEventCount == 0
			&& KRemoteKeyboardRoutingServiceTestAccess::pendingModifierCount(&service) == 2,
			QStringLiteral("shortcut modifiers remain pending"));
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, 'F', true, 0x21);
		QCoreApplication::processEvents();
		check(nRemoteEventCount == 0 && nFullscreenCount == 1,
			QStringLiteral("function shortcut stays local"));
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, 'F', false, 0x21);
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, VK_LMENU, false, 0x38);
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, VK_LCONTROL, false, 0x1D);
		check(nRemoteEventCount == 0,
			QStringLiteral("consumed shortcut releases stay local"));
	}

	void testOrdinaryShortcutFlushesInOrder()
	{
		auto spStore = std::make_unique<KMemoryKeyboardSettingsStore>();
		KKeyboardSettingsService settingsService(std::move(spStore));
		settingsService.initialize();
		KRemoteKeyboardRoutingService service(&settingsService);
		KRemoteKeyboardRoutingServiceTestAccess::enableCapture(&service);
		QStringList events;
		QObject::connect(&service, &KRemoteKeyboardRoutingService::remoteKeyRequested,
			[&events](int nVirtualKey, int, bool bPressed, bool, bool)
			{
				events.append(QStringLiteral("%1:%2")
					.arg(nVirtualKey).arg(bPressed ? QStringLiteral("down") : QStringLiteral("up")));
			});
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, VK_LCONTROL, true, 0x1D);
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, 'A', true, 0x1E);
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, 'A', false, 0x1E);
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, VK_LCONTROL, false, 0x1D);
		check(events == QStringList({ QStringLiteral("162:down"),
			QStringLiteral("65:down"), QStringLiteral("65:up"),
			QStringLiteral("162:up") }),
			QStringLiteral("ordinary shortcut flushes modifiers before the main key"));
	}

	void testMouseFlushesPendingModifiers()
	{
		auto spStore = std::make_unique<KMemoryKeyboardSettingsStore>();
		KKeyboardSettingsService settingsService(std::move(spStore));
		settingsService.initialize();
		KRemoteKeyboardRoutingService service(&settingsService);
		KRemoteKeyboardRoutingServiceTestAccess::enableCapture(&service);
		int nRemoteEventCount = 0;
		QObject::connect(&service, &KRemoteKeyboardRoutingService::remoteKeyRequested,
			[&nRemoteEventCount](int, int, bool, bool, bool) { ++nRemoteEventCount; });
		KRemoteKeyboardRoutingServiceTestAccess::route(&service, VK_LSHIFT, true, 0x2A);
		service.flushPendingModifiers();
		check(nRemoteEventCount == 1
			&& KRemoteKeyboardRoutingServiceTestAccess::pendingModifierCount(&service) == 0,
			QStringLiteral("mouse boundary flushes pending modifier down"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testFunctionShortcutDoesNotReachRemote();
	testOrdinaryShortcutFlushesInOrder();
	testMouseFlushesPendingModifiers();
	if (g_nFailureCount == 0)
		qInfo() << "All remote keyboard routing tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
