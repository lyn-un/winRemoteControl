#ifndef _WINREMOTECONTROL_STREAMCONFIG_H_
#define _WINREMOTECONTROL_STREAMCONFIG_H_

#include <QtCore/QMetaType>

struct KStreamConfig
{
	int nFps = 30;
	int nWidth = 1280;
	int nHeight = 720;
	int nBitrateKbps = 3000;
};

Q_DECLARE_METATYPE(KStreamConfig)

#endif // _WINREMOTECONTROL_STREAMCONFIG_H_
