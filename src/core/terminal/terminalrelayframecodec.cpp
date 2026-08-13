#include "core/terminal/terminalrelayframecodec.h"

#include "core/terminal/terminalrelayprotocol.h"

QByteArray KTerminalRelayFrameCodec::encode(
	quint16 nType,
	const QByteArray &payload)
{
	if (payload.size() > KTerminalRelayProtocol::kMaximumPayloadBytes)
		return {};
	if (!KTerminalRelayProtocol::IsKnownFrameType(nType))
		return {};
	const auto header = KTerminalRelayProtocol::EncodeFrameHeader(nType,
		static_cast<quint32>(payload.size()));
	QByteArray frame(reinterpret_cast<const char *>(header.data()), header.size());
	frame.append(payload);
	return frame;
}

bool KTerminalRelayFrameCodec::append(
	const QByteArray &data,
	QVector<KTerminalRelayFrame> *pFrames,
	QString *pErrorMessage)
{
	if (pFrames == nullptr)
		return false;
	constexpr qsizetype kMaximumBufferedBytes = 1024 * 1024;
	if (data.size() > kMaximumBufferedBytes - m_buffer.size())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("terminal relay receive buffer overflow");
		m_buffer.clear();
		return false;
	}
	m_buffer.append(data);
	while (m_buffer.size() >= static_cast<qsizetype>(
		KTerminalRelayProtocol::kFrameHeaderBytes))
	{
		KTerminalRelayProtocol::DecodedFrameHeader header;
		if (!KTerminalRelayProtocol::DecodeFrameHeader(
				reinterpret_cast<const std::uint8_t *>(m_buffer.constData()), &header)
			|| !KTerminalRelayProtocol::IsKnownFrameType(header.nType)
			|| header.nPayloadBytes > KTerminalRelayProtocol::kMaximumPayloadBytes)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("invalid terminal relay frame header");
			m_buffer.clear();
			return false;
		}
		const qsizetype nFrameBytes = KTerminalRelayProtocol::kFrameHeaderBytes
			+ header.nPayloadBytes;
		if (m_buffer.size() < nFrameBytes)
			return true;
		KTerminalRelayFrame frame;
		frame.nType = header.nType;
		frame.payload = m_buffer.mid(KTerminalRelayProtocol::kFrameHeaderBytes,
			header.nPayloadBytes);
		pFrames->append(std::move(frame));
		m_buffer.remove(0, nFrameBytes);
	}
	return true;
}

void KTerminalRelayFrameCodec::clear()
{
	m_buffer.clear();
}
