#ifndef _WINREMOTECONTROL_CORE_SETTINGS_DEVICESECURITYPREFERENCESTORE_H_
#define _WINREMOTECONTROL_CORE_SETTINGS_DEVICESECURITYPREFERENCESTORE_H_

#include "core/settings/devicesecuritypreference.h"

#include <QtCore/QVector>

class KDeviceSecurityPreferenceStore
{
public:
	virtual ~KDeviceSecurityPreferenceStore() = default;

	virtual QVector<KDeviceSecurityPreference> load(QString *pError) = 0;
	virtual bool save(const QVector<KDeviceSecurityPreference> &preferences,
		QString *pError) = 0;
};

#endif // _WINREMOTECONTROL_CORE_SETTINGS_DEVICESECURITYPREFERENCESTORE_H_
