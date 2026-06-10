#ifndef _WINREMOTECONTROL_STREAMCONFIG_H_
#define _WINREMOTECONTROL_STREAMCONFIG_H_

#include <QtCore/QMetaType>

struct KStreamConfig
{
	int nFps = 30;
	int nWidth = 0;
	int nHeight = 0;
	int nBitrateKbps = 2000;
};

Q_DECLARE_METATYPE(KStreamConfig)

#endif // _WINREMOTECONTROL_STREAMCONFIG_H_
