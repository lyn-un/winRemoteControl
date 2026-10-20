#ifndef _WINREMOTECONTROL_H264HARDWAREBITRATETEST_H_
#define _WINREMOTECONTROL_H264HARDWAREBITRATETEST_H_

#include "codec/h264encoder.h"

bool TestHardwareBitrateOutput(KVideoEncoderPreference preference, KNvencReconfiguration configuration = {});
bool TestHardwareFrequentRates(KVideoEncoderPreference preference, KNvencReconfiguration configuration = {});
bool TestNvencKeyframePolicy(KNvencReconfiguration configuration);

#endif // _WINREMOTECONTROL_H264HARDWAREBITRATETEST_H_
