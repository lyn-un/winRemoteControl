#ifndef _WINREMOTECONTROL_WEBRTCH264FORMAT_H_
#define _WINREMOTECONTROL_WEBRTCH264FORMAT_H_

#include "codec/h264levelpolicy.h"

#include <api/video_codecs/sdp_video_format.h>
#include <QtCore/QString>
#include <span>
#include <cstdint>

webrtc::SdpVideoFormat SupportedH264Format();
int NegotiatedH264Level(const webrtc::SdpVideoFormat &format);

class KH264SpsGuard
{
public:
	explicit KH264SpsGuard(int nMaximumLevel = KH264LevelPolicy::kMaximumLevel);
	void reset();
	int observedLevel() const;
	bool validate(std::span<const std::uint8_t> annexB, int nWidth, int nHeight, int nFps, QString *pError);

private:
	int m_nMaximumLevel;
	bool m_bSeenSps = false;
	bool m_bFailed = false;
	int m_nObservedLevel = 0;
};

#endif // _WINREMOTECONTROL_WEBRTCH264FORMAT_H_
