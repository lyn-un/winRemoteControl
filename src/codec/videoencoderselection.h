#ifndef _WINREMOTECONTROL_VIDEOENCODERSELECTION_H_
#define _WINREMOTECONTROL_VIDEOENCODERSELECTION_H_

#include "core/media/videoencoderpreference.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <functional>
#include <vector>

struct KVideoAdapter
{
	int nIndex = -1;
	quint32 nVendorId = 0;
	quint32 nDeviceId = 0;
	quint64 nLuid = 0;
	QString strName;
	bool bSoftware = false;
};

struct KVideoEncoderCandidate
{
	QString strEncoder;
	KVideoAdapter adapter;
};

QString VideoEncoderPreferenceName(KVideoEncoderPreference preference);

std::vector<KVideoEncoderCandidate> BuildVideoEncoderCandidates(
	KVideoEncoderPreference preference, const std::vector<KVideoAdapter> &adapters);

// The callback owns initialization and releases each failed codec/device before returning.
bool TryVideoEncoderCandidates(const std::vector<KVideoEncoderCandidate> &candidates,
	const std::function<bool(const KVideoEncoderCandidate &, QString *)> &openCandidate,
	KVideoEncoderCandidate *pSelected, QStringList *pFailures);

#endif // _WINREMOTECONTROL_VIDEOENCODERSELECTION_H_
