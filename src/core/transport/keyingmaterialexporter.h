#ifndef _WINREMOTECONTROL_CORE_TRANSPORT_KEYINGMATERIALEXPORTER_H_
#define _WINREMOTECONTROL_CORE_TRANSPORT_KEYINGMATERIALEXPORTER_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

class KKeyingMaterialExporter
{
public:
	virtual ~KKeyingMaterialExporter() = default;

	virtual bool exportKeyingMaterial(const QByteArray &label,
		const QByteArray &context,
		int nLength,
		QByteArray *pKeyingMaterial,
		QString *pErrorMessage) = 0;
};

#endif // _WINREMOTECONTROL_CORE_TRANSPORT_KEYINGMATERIALEXPORTER_H_
