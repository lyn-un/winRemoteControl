#include "transport/webrtc/webrtch264format.h"

#include <api/video_codecs/h264_profile_level_id.h>
#include <common_video/h264/h264_common.h>
#include <common_video/h264/sps_parser.h>
#include <QtCore/QByteArray>
#include <algorithm>

webrtc::SdpVideoFormat SupportedH264Format()
{
	const auto profile = webrtc::H264ProfileLevelIdToString({
		webrtc::H264Profile::kProfileConstrainedBaseline,
		static_cast<webrtc::H264Level>(KH264LevelPolicy::kMaximumLevel)});
	// Symmetric negotiation avoids advertising one direction's receive ceiling
	// as the other direction's send permission. Older asymmetry=1 peers still negotiate.
	return webrtc::SdpVideoFormat("H264", {{"profile-level-id", *profile},
		{"level-asymmetry-allowed", "0"}, {"packetization-mode", "1"}});
}

int NegotiatedH264Level(const webrtc::SdpVideoFormat &format)
{
	if (QString::fromStdString(format.name).compare(QStringLiteral("H264"), Qt::CaseInsensitive) != 0)
		return 0;
	const auto mode = format.parameters.find("packetization-mode");
	if (mode == format.parameters.end() || mode->second != "1")
		return 0;
	const auto profile = webrtc::ParseSdpForH264ProfileLevelId(format.parameters);
	if (!profile || profile->profile != webrtc::H264Profile::kProfileConstrainedBaseline)
		return 0;
	const int nLevel = static_cast<int>(profile->level);
	return KH264LevelPolicy::isSupported(nLevel) ? nLevel : 0;
}

KH264SpsGuard::KH264SpsGuard(int nMaximumLevel)
	: m_nMaximumLevel(nMaximumLevel)
{
}

void KH264SpsGuard::reset()
{
	m_bSeenSps = false;
	m_bFailed = false;
	m_nObservedLevel = 0;
}

int KH264SpsGuard::observedLevel() const
{
	return m_nObservedLevel;
}

bool KH264SpsGuard::validate(std::span<const std::uint8_t> annexB,
	int nWidth, int nHeight, int nFps, QString *pError)
{
	const auto Fail = [this, pError](const QString &error)
	{
		m_bFailed = true;
		if (pError)
			*pError = error;
		return false;
	};
	if (m_bFailed)
		return Fail(QStringLiteral("H.264 compatibility failure is latched; reinitialize the session"));
	const auto units = webrtc::H264::FindNaluIndices(annexB);
	if (units.empty())
		return Fail(QStringLiteral("H.264 access unit has no Annex-B NAL units"));
	for (const auto &unit : units)
	{
		if (unit.payload_size < 1)
			return Fail(QStringLiteral("Empty H.264 NAL unit"));
		const auto payload = annexB.subspan(unit.payload_start_offset, unit.payload_size);
		const auto type = webrtc::H264::ParseNaluType(payload.front());
		if (type == webrtc::H264::kSps)
		{
			if (payload.size() < 4)
				return Fail(QStringLiteral("Truncated H.264 SPS"));
			const auto rbsp = webrtc::H264::ParseRbsp(payload.subspan(1));
			if (rbsp.size() < 3)
				return Fail(QStringLiteral("Truncated H.264 SPS header"));
			const QByteArray profileBytes(reinterpret_cast<const char *>(rbsp.data()), 3);
			const auto profile = webrtc::ParseH264ProfileLevelId(profileBytes.toHex().constData());
			const auto sps = webrtc::SpsParser::ParseSps(payload.subspan(1));
			if (!profile || !sps || profile->profile != webrtc::H264Profile::kProfileConstrainedBaseline
				|| !sps->frame_mbs_only_flag || sps->max_num_ref_frames > 16)
				return Fail(QStringLiteral("Invalid or unsupported H.264 SPS profile/structure: "
					"profileLevelId=%1 profileParsed=%2 spsParsed=%3 frameMbsOnly=%4 maxRefFrames=%5")
					.arg(QString::fromLatin1(profileBytes.toHex())).arg(profile.has_value()).arg(sps.has_value())
					.arg(sps ? static_cast<int>(sps->frame_mbs_only_flag) : -1)
					.arg(sps ? static_cast<int>(sps->max_num_ref_frames) : -1));
			const int nLevel = static_cast<int>(profile->level);
			if (nLevel > m_nMaximumLevel || !KH264LevelPolicy::fits(nLevel,
				static_cast<int>(sps->width), static_cast<int>(sps->height), std::max(1, nFps),
				1, 0, static_cast<int>(sps->max_num_ref_frames)))
				return Fail(QStringLiteral("H.264 SPS level=%1 exceeds negotiated level=%2 or cannot contain the stream")
					.arg(nLevel).arg(m_nMaximumLevel));
			if ((nWidth > 0 && sps->width != static_cast<unsigned>(nWidth))
				|| (nHeight > 0 && sps->height != static_cast<unsigned>(nHeight)))
				return Fail(QStringLiteral("H.264 SPS dimensions differ from configured dimensions"));
			m_bSeenSps = true;
			m_nObservedLevel = nLevel;
		}
		else if ((type == webrtc::H264::kSlice || type == webrtc::H264::kIdr) && !m_bSeenSps)
		{
			// A receiver may join after an IDR was lost. Permit a later SPS to recover.
			if (pError)
				*pError = QStringLiteral("H.264 slice arrived before a validated SPS; waiting for keyframe");
			return false;
		}
	}
	return true;
}
