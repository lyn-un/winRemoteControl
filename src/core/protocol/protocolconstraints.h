#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLCONSTRAINTS_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLCONSTRAINTS_H_

class KProtocolConstraints
{
public:
	static constexpr int kEnvelopeSchemaVersion = 1;
	static constexpr int kSessionProtocolMinVersion = 2;
	static constexpr int kSessionProtocolMaxVersion = 2;
	static constexpr int kMaximumRequestIdCharacters = 64;
	static constexpr int kMaximumAccessMessageBytes = 2048;
	static constexpr int kMaximumInputMessageBytes = 4096;
	static constexpr int kMaximumSessionMessageBytes = 160 * 1024;
	static constexpr int kMaximumClipboardMessageBytes = 2 * 1024 * 1024;
	static constexpr int kMaximumFileControlMessageBytes = 128 * 1024;
	static constexpr int kMaximumSignalingMessageBytes = 256 * 1024;
	static constexpr int kMaximumDataChannelMessageBytes = kMaximumClipboardMessageBytes;
	static constexpr int kMaximumWallpaperBase64Bytes = 96 * 1024;
	static constexpr int kMaximumComputerNameCharacters = 128;
	static constexpr int kMaximumReasonCharacters = 128;
	static constexpr int kMaximumScreenDimension = 32768;
	static constexpr int kMaximumMouseCoordinate = 32767;
	static constexpr int kMaximumWheelDelta = 12000;
	static constexpr int kMaximumTextInputBytes = 2048;
	static constexpr int kMinimumStreamFps = 1;
	static constexpr int kMaximumStreamFps = 60;
	static constexpr int kMaximumStreamWidth = 7680;
	static constexpr int kMaximumStreamHeight = 4320;
	static constexpr int kMinimumStreamBitrateKbps = 500;
	static constexpr int kMaximumStreamBitrateKbps = 50000;
	static constexpr int kMaximumInvalidMessages = 3;
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLCONSTRAINTS_H_
