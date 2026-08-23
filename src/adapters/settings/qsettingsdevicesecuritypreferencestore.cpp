#include "adapters/settings/qsettingsdevicesecuritypreferencestore.h"

#include "common/sessiontracelogger.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QUuid>

#include <algorithm>

namespace
{
	constexpr int kSchemaVersion = 1;
	constexpr int kMaximumPreferences = 128;
	constexpr char kPreferencesArray[] = "deviceSecurityPreferences";

	bool IsValidDeviceId(const QString &strDeviceId)
	{
		return !strDeviceId.isEmpty() && !QUuid(strDeviceId).isNull();
	}

	void SortAndTrim(QVector<KDeviceSecurityPreference> *pPreferences)
	{
		std::sort(pPreferences->begin(), pPreferences->end(),
			[](const KDeviceSecurityPreference &left,
				const KDeviceSecurityPreference &right)
			{
				return left.nUpdatedAtMs > right.nUpdatedAtMs;
			});
		if (pPreferences->size() > kMaximumPreferences)
			pPreferences->resize(kMaximumPreferences);
	}
}

KQSettingsDeviceSecurityPreferenceStore::KQSettingsDeviceSecurityPreferenceStore(
	const QString &strFilePath)
	: m_strFilePath(strFilePath)
{
}

KQSettingsDeviceSecurityPreferenceStore::~KQSettingsDeviceSecurityPreferenceStore()
{
}

QVector<KDeviceSecurityPreference> KQSettingsDeviceSecurityPreferenceStore::load(
	QString *pError)
{
	QVector<KDeviceSecurityPreference> preferences;
	QSettings settings(m_strFilePath, QSettings::IniFormat);
	const int nSchemaVersion = settings.value(QStringLiteral("meta/schemaVersion"),
		kSchemaVersion).toInt();
	if (nSchemaVersion != kSchemaVersion)
	{
		if (pError != nullptr)
			*pError = QStringLiteral("不支持的设备安全偏好配置版本");
		return preferences;
	}

	const int nCount = settings.beginReadArray(QString::fromLatin1(kPreferencesArray));
	for (int nIndex = 0; nIndex < nCount; ++nIndex)
	{
		settings.setArrayIndex(nIndex);
		KDeviceSecurityPreference preference;
		preference.strRemoteDeviceId = settings.value(QStringLiteral("deviceId"))
			.toString().trimmed();
		if (!IsValidDeviceId(preference.strRemoteDeviceId))
		{
			KSessionTraceLogger::write(QStringLiteral("controller"),
				QStringLiteral("device_preference_loaded"), QStringLiteral("dropped"), -1,
				QStringLiteral("reason=invalid_device_id"));
			continue;
		}

		bool bUseSafeDefaults = false;
		const QString strPrivacyMode = settings.value(QStringLiteral("privacyMode"),
			QStringLiteral("disabled")).toString();
		if (!DeviceSecurityPrivacyModeFromName(strPrivacyMode,
			&preference.desiredPrivacyMode))
		{
			bUseSafeDefaults = true;
		}
		const QString strPostSessionAction = settings.value(
			QStringLiteral("postSessionAction"), QStringLiteral("none")).toString();
		if (!DeviceSecurityPostSessionActionFromName(strPostSessionAction,
			&preference.desiredPostSessionAction))
		{
			bUseSafeDefaults = true;
		}
		bool bTimestampValid = false;
		preference.nUpdatedAtMs = settings.value(QStringLiteral("updatedAtMs"), 0)
			.toLongLong(&bTimestampValid);
		if (!bTimestampValid || preference.nUpdatedAtMs < 0)
			bUseSafeDefaults = true;
		if (bUseSafeDefaults)
		{
			preference.desiredPrivacyMode = DisabledPrivacyMode;
			preference.desiredPostSessionAction = NoPostSessionAction;
			preference.nUpdatedAtMs = 0;
			KSessionTraceLogger::write(QStringLiteral("controller"),
				QStringLiteral("device_preference_loaded"), QStringLiteral("sanitized"), -1,
				QStringLiteral("deviceId=%1 reason=invalid_fields")
					.arg(preference.strRemoteDeviceId.left(12)));
		}
		preferences.append(preference);
	}
	settings.endArray();
	SortAndTrim(&preferences);
	if (settings.status() != QSettings::NoError && pError != nullptr)
		*pError = QStringLiteral("读取设备安全偏好失败");
	return preferences;
}

bool KQSettingsDeviceSecurityPreferenceStore::save(
	const QVector<KDeviceSecurityPreference> &preferences,
	QString *pError)
{
	const QFileInfo fileInfo(m_strFilePath);
	if (!QDir().mkpath(fileInfo.absolutePath()))
	{
		if (pError != nullptr)
			*pError = QStringLiteral("无法创建设备安全偏好目录");
		return false;
	}

	QVector<KDeviceSecurityPreference> storedPreferences;
	for (const KDeviceSecurityPreference &preference : preferences)
	{
		if (IsValidDeviceId(preference.strRemoteDeviceId))
			storedPreferences.append(preference);
	}
	SortAndTrim(&storedPreferences);

	QSettings settings(m_strFilePath, QSettings::IniFormat);
	settings.setValue(QStringLiteral("meta/schemaVersion"), kSchemaVersion);
	settings.remove(QString::fromLatin1(kPreferencesArray));
	settings.beginWriteArray(QString::fromLatin1(kPreferencesArray),
		storedPreferences.size());
	for (int nIndex = 0; nIndex < storedPreferences.size(); ++nIndex)
	{
		const KDeviceSecurityPreference &preference = storedPreferences.at(nIndex);
		settings.setArrayIndex(nIndex);
		settings.setValue(QStringLiteral("deviceId"), preference.strRemoteDeviceId);
		settings.setValue(QStringLiteral("privacyMode"),
			DeviceSecurityPrivacyModeName(preference.desiredPrivacyMode));
		settings.setValue(QStringLiteral("postSessionAction"),
			DeviceSecurityPostSessionActionName(preference.desiredPostSessionAction));
		settings.setValue(QStringLiteral("updatedAtMs"), preference.nUpdatedAtMs);
	}
	settings.endArray();
	settings.sync();
	if (settings.status() == QSettings::NoError)
		return true;
	if (pError != nullptr)
		*pError = QStringLiteral("写入设备安全偏好失败");
	return false;
}
