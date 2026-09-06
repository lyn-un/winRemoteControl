#include "adapters/settings/qsettingskeyboardstore.h"
#include "settings/keyboardsettingsservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QTemporaryDir>

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

	void testCanonicalRoundTrip()
	{
		KKeyboardShortcut shortcut;
		check(KeyboardShortcutFromCanonicalName(QStringLiteral("Alt+Ctrl+F"), &shortcut),
			QStringLiteral("shortcut parser accepts modifier order"));
		check(KeyboardShortcutCanonicalName(shortcut) == QStringLiteral("Ctrl+Alt+F"),
			QStringLiteral("shortcut output uses stable modifier order"));
		check(KeyboardShortcutDisplayName(shortcut) == QStringLiteral("Ctrl + Alt + F"),
			QStringLiteral("shortcut display name uses keycap spacing"));
		check(!KeyboardShortcutFromCanonicalName(QStringLiteral("Ctrl+Alt"), &shortcut),
			QStringLiteral("bare modifiers are rejected"));
		KKeyboardShortcut secure;
		check(KeyboardShortcutFromCanonicalName(QStringLiteral("Ctrl+Alt+Delete"), &secure)
			&& IsSecureAttentionShortcut(secure),
			QStringLiteral("secure attention shortcut is detected"));
	}

	void testPersistenceAndValidation()
	{
		QTemporaryDir temporaryDir;
		const QString strPath = temporaryDir.filePath(QStringLiteral("settings.ini"));
		auto spStore = std::make_unique<KQSettingsKeyboardStore>(strPath);
		KKeyboardSettingsService service(std::move(spStore));
		service.initialize();
		check(service.settings().localOnlyShortcuts.isEmpty()
			&& service.settings().toggleFullscreenShortcut
				== DefaultToggleFullscreenShortcut(),
			QStringLiteral("keyboard settings load safe defaults"));

		int nErrorCount = 0;
		int nChangedCount = 0;
		QObject::connect(&service, &KKeyboardSettingsService::settingsError,
			[&nErrorCount](const QString &strError)
			{
				qCritical().noquote() << QStringLiteral("keyboard settings error: %1").arg(strError);
				++nErrorCount;
			});
		QObject::connect(&service, &KKeyboardSettingsService::settingsChanged,
			[&nChangedCount](const KKeyboardSettings &) { ++nChangedCount; });
		service.addLocalShortcut(QStringLiteral("Win+D"));
		check(nChangedCount == 1 && service.settings().localOnlyShortcuts.size() == 1,
			QStringLiteral("valid local shortcut is stored"));
		service.addLocalShortcut(QStringLiteral("Win+D"));
		service.addLocalShortcut(QStringLiteral("Ctrl+Alt+Delete"));
		service.addLocalShortcut(QStringLiteral("Ctrl+Alt+Enter"));
		check(nErrorCount == 3 && service.settings().localOnlyShortcuts.size() == 1,
			QStringLiteral("duplicate, secure and release shortcuts are rejected"));

		KQSettingsKeyboardStore roundTripStore(strPath);
		KKeyboardSettings roundTrip;
		check(roundTripStore.loadSettings(&roundTrip, nullptr)
			&& roundTrip.localOnlyShortcuts.size() == 1
			&& KeyboardShortcutCanonicalName(roundTrip.localOnlyShortcuts.constFirst())
				== QStringLiteral("Win+D"),
			QStringLiteral("keyboard settings round-trip"));
		service.restoreDefaults();
		check(service.settings().localOnlyShortcuts.isEmpty(),
			QStringLiteral("restore defaults clears local shortcuts"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testCanonicalRoundTrip();
	testPersistenceAndValidation();
	if (g_nFailureCount == 0)
		qInfo() << "All keyboard settings tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
