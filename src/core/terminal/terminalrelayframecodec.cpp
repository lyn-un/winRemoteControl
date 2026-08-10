#include "core/terminal/terminalrelayframecodec.h"

#include "core/terminal/terminalrelayprotocol.h"

#include <cstring>

QByteArray KTerminalRelayFrameCodec::encode(
	quint16 nType,
	const QByteArray &payload)
{
	if (payload.size() > KTerminalRelayProtocol::kMaximumPayloadBytes)
		return {};
	KTerminalRelayProtocol::FrameHeader header;
	header.nType = nType;
	header.nPayloadBytes = static_cast<quint32>(payload.size());
	QByteArray frame(reinterpret_cast<const char *>(&header), sizeof(header));
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
	m_buffer.append(data);
	while (m_buffer.size() >= static_cast<qsizetype>(
		sizeof(KTerminalRelayProtocol::FrameHeader)))
	{
		KTerminalRelayProtocol::FrameHeader header;
		std::memcpy(&header, m_buffer.constData(), sizeof(header));
		if (header.nMagic != KTerminalRelayProtocol::kMagic
			|| header.nVersion != KTerminalRelayProtocol::kVersion
			|| header.nPayloadBytes > KTerminalRelayProtocol::kMaximumPayloadBytes)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("invalid terminal relay frame header");
			m_buffer.clear();
			return false;
		}
		const qsizetype nFrameBytes = sizeof(header) + header.nPayloadBytes;
		if (m_buffer.size() < nFrameBytes)
			return true;
		KTerminalRelayFrame frame;
		frame.nType = header.nType;
		frame.payload = m_buffer.mid(sizeof(header), header.nPayloadBytes);
		pFrames->append(std::move(frame));
		m_buffer.remove(0, nFrameBytes);
	}
	return true;
}

void KTerminalRelayFrameCodec::clear()
{
	m_buffer.clear();
}
