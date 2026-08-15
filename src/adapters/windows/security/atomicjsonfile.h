#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_ATOMICJSONFILE_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_ATOMICJSONFILE_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

class KAtomicJsonFile
{
public:
	static bool write(const QString &strFilePath,
		const QByteArray &data,
		QString *pErrorMessage);
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_ATOMICJSONFILE_H_
