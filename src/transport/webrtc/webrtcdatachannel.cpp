#include "transport/webrtc/webrtcdatachannel.h"

#include <QtCore/QByteArray>

#include <string>
#include <utility>

KWebRtcDataChannel::KWebRtcDataChannel(QObject *pParent)
	: QObject(pParent)
{
}

KWebRtcDataChannel::~KWebRtcDataChannel()
{
	clear();
}

void KWebRtcDataChannel::setChannel(
	webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel)
{
	clear();
	m_spChannel = std::move(spChannel);
	if (m_spChannel == nullptr)
		return;

	m_spChannel->RegisterObserver(this);
	emit openChanged(isOpen());
}

void KWebRtcDataChannel::clear()
{
	const bool bWasOpen = isOpen();
	if (m_spChannel != nullptr)
		m_spChannel->UnregisterObserver();
	m_spChannel = nullptr;
	if (bWasOpen)
		emit openChanged(false);
}

bool KWebRtcDataChannel::isOpen() const
{
	return m_spChannel != nullptr
		&& m_spChannel->state() == webrtc::DataChannelInterface::kOpen;
}

bool KWebRtcDataChannel::sendText(const QString &strMessage)
{
	if (!isOpen())
		return false;

	const QByteArray utf8Message = strMessage.toUtf8();
	webrtc::DataBuffer buffer(std::string(utf8Message.constData(),
		static_cast<size_t>(utf8Message.size())));
	return m_spChannel->Send(buffer);
}

void KWebRtcDataChannel::OnStateChange()
{
	emit openChanged(isOpen());
}

void KWebRtcDataChannel::OnMessage(const webrtc::DataBuffer &buffer)
{
	if (buffer.binary)
		return;

	const char *pData = reinterpret_cast<const char *>(buffer.data.data());
	emit textMessageReceived(
		QString::fromUtf8(pData, static_cast<int>(buffer.data.size())));
}

void KWebRtcDataChannel::OnBufferedAmountChange(uint64_t)
{
}
