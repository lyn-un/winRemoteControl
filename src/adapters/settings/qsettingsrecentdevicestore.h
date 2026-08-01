#ifndef _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSRECENTDEVICESTORE_H_
#define _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSRECENTDEVICESTORE_H_

#include "core/devices/recentdevicestore.h"

class KQSettingsRecentDeviceStore final : public KRecentDeviceStore
{
public:
	explicit KQSettingsRecentDeviceStore(const QString &strFilePath);
	~KQSettingsRecentDeviceStore() override;

	QVector<KRecentDevice> loadDevices(QString *pError) override;
	bool saveDevices(const QVector<KRecentDevice> &devices, QString *pError) override;

private:
	QString m_strFilePath;
};

#endif // _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSRECENTDEVICESTORE_H_
