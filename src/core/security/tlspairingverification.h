#ifndef _WINREMOTECONTROL_CORE_SECURITY_TLSPAIRINGVERIFICATION_H_
#define _WINREMOTECONTROL_CORE_SECURITY_TLSPAIRINGVERIFICATION_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

class KTlsPairingVerification
{
public:
	static constexpr int kKeyingMaterialBytes = 32;

	static QString verificationMethod();
	static QByteArray exporterLabel();
	static QByteArray createContext(const QString &strRequestId,
		const QString &strControllerDeviceId,
		const QString &strControlledDeviceId,
		const QByteArray &controllerSpkiSha256,
		const QByteArray &controlledSpkiSha256,
		QString *pErrorMessage);
	static QString numericCode(const QByteArray &keyingMaterial,
		QString *pErrorMessage);
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_TLSPAIRINGVERIFICATION_H_
