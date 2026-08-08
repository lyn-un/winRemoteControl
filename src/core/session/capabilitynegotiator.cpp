#include "core/session/capabilitynegotiator.h"

#include <algorithm>

bool KCapabilityNegotiationResult::succeeded() const
{
	return status == CapabilityNegotiationSucceeded && capabilities.bValid;
}

KCapabilityNegotiationResult KCapabilityNegotiator::negotiate(
	const KSessionCapabilities &localCapabilities,
	const KSessionCapabilities &remoteCapabilities)
{
	KCapabilityNegotiationResult result;
	const int nMinimumVersion = std::max(localCapabilities.nProtocolMinVersion,
		remoteCapabilities.nProtocolMinVersion);
	const int nMaximumVersion = std::min(localCapabilities.nProtocolMaxVersion,
		remoteCapabilities.nProtocolMaxVersion);
	if (nMinimumVersion > nMaximumVersion)
	{
		result.status = IncompatibleVersionCapabilityNegotiationStatus;
		result.strTechnicalMessage = QStringLiteral("No compatible session protocol version");
		return result;
	}
	if (!localCapabilities.supportedCodecs.contains(QStringLiteral("h264"))
		|| !remoteCapabilities.supportedCodecs.contains(QStringLiteral("h264")))
	{
		result.status = MissingCodecCapabilityNegotiationStatus;
		result.strTechnicalMessage = QStringLiteral("H.264 is not supported by both peers");
		return result;
	}
	for (const QString &strRequiredChannel : {
		QStringLiteral("video"), QStringLiteral("session"), QStringLiteral("input") })
	{
		if (!localCapabilities.supportedChannels.contains(strRequiredChannel)
			|| !remoteCapabilities.supportedChannels.contains(strRequiredChannel))
		{
			result.status = MissingChannelCapabilityNegotiationStatus;
			result.strTechnicalMessage = QStringLiteral("Missing required channel: %1")
				.arg(strRequiredChannel);
			return result;
		}
	}

	KNegotiatedCapabilities &negotiated = result.capabilities;
	negotiated.bValid = true;
	negotiated.nProtocolVersion = nMaximumVersion;
	negotiated.strVideoCodec = QStringLiteral("h264");
	for (const QString &strChannel : localCapabilities.supportedChannels)
	{
		if (remoteCapabilities.supportedChannels.contains(strChannel))
			negotiated.channels.append(strChannel);
	}
	negotiated.nMaximumWidth = std::min(localCapabilities.nMaximumWidth,
		remoteCapabilities.nMaximumWidth);
	negotiated.nMaximumHeight = std::min(localCapabilities.nMaximumHeight,
		remoteCapabilities.nMaximumHeight);
	negotiated.nMaximumFps = std::min(localCapabilities.nMaximumFps,
		remoteCapabilities.nMaximumFps);
	negotiated.nMaximumBitrateKbps = std::min(localCapabilities.nMaximumBitrateKbps,
		remoteCapabilities.nMaximumBitrateKbps);
	negotiated.bClipboardText = localCapabilities.bClipboardText
		&& remoteCapabilities.bClipboardText
		&& negotiated.channels.contains(QStringLiteral("clipboard"));
	negotiated.bInputRealtime = localCapabilities.bInputRealtime
		&& remoteCapabilities.bInputRealtime
		&& negotiated.channels.contains(QStringLiteral("input-realtime"));
	negotiated.bKeyboard = localCapabilities.bKeyboard && remoteCapabilities.bKeyboard;
	negotiated.bUnicodeText = localCapabilities.bUnicodeText && remoteCapabilities.bUnicodeText;
	negotiated.bMouseButtons = localCapabilities.bMouseButtons && remoteCapabilities.bMouseButtons;
	negotiated.bMouseWheel = localCapabilities.bMouseWheel && remoteCapabilities.bMouseWheel;
	result.status = CapabilityNegotiationSucceeded;
	return result;
}
