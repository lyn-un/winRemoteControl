#ifndef _WINREMOTECONTROL_FRAMEWATERMARK_H_
#define _WINREMOTECONTROL_FRAMEWATERMARK_H_

#include <QtCore/QtGlobal>

#include <vector>

struct KFrameWatermark
{
	quint64 nSourceFrameIndex = 0;
	quint64 nLastInputSeq = 0;
	qint64 nInputAgeMs = -1;
};

class KFrameWatermarkCodec
{
public:
	static bool writeBgra(std::vector<unsigned char> *pBgraBuffer,
		int nWidth,
		int nHeight,
		const KFrameWatermark &watermark);
	static bool readBgra(const std::vector<unsigned char> &bgraBuffer,
		int nWidth,
		int nHeight,
		KFrameWatermark *pWatermark);
	static bool removeBgra(std::vector<unsigned char> *pBgraBuffer,
		int nWidth,
		int nHeight);
};

#endif // _WINREMOTECONTROL_FRAMEWATERMARK_H_
