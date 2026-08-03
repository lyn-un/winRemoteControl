#include "transport/webrtc/webrtcdatachannel.h"

#include <QtCore/QByteArray>

#include <string>
#include <utility>

KWebRtcDataChannel::KWebRtcDataChannel(int nMaximumMessageBytes, QObject *pParent)
	: QObject(pParent)
	, m_nMaximumMessageBytes(nMaximumMessageBytes)
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
	m_bBackpressured = false;
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
	if (utf8Message.size() > m_nMaximumMessageBytes)
		return false;
	webrtc::DataBuffer buffer(std::string(utf8Message.constData(),
		static_cast<size_t>(utf8Message.size())));
	const bool bSent = m_spChannel->Send(buffer);
	const quint64 nBufferedBytes = bufferedAmount();
	if (m_nHighWatermarkBytes > 0 && nBufferedBytes >= m_nHighWatermarkBytes)
		m_bBackpressured = true;
	emit bufferedAmountChanged(nBufferedBytes);
	return bSent;
}

void KWebRtcDataChannel::setBufferWatermarks(quint64 nLowBytes, quint64 nHighBytes)
{
	m_nLowWatermarkBytes = nLowBytes;
	m_nHighWatermarkBytes = qMax(nLowBytes, nHighBytes);
}

quint64 KWebRtcDataChannel::bufferedAmount() const
{
	return m_spChannel != nullptr ? m_spChannel->buffered_amount() : 0;
}

bool KWebRtcDataChannel::isBackpressured() const
{
	return m_bBackpressured;
}

void KWebRtcDataChannel::OnStateChange()
{
	emit openChanged(isOpen());
}

void KWebRtcDataChannel::OnMessage(const webrtc::DataBuffer &buffer)
{
	if (buffer.binary)
	{
		emit messageRejected(static_cast<int>(buffer.data.size()),
			QStringLiteral("Binary DataChannel message is not supported"));
		return;
	}
	if (buffer.data.size() > static_cast<size_t>(m_nMaximumMessageBytes))
	{
		emit messageRejected(static_cast<int>(buffer.data.size()),
			QStringLiteral("DataChannel message is too large"));
		return;
	}

	const char *pData = reinterpret_cast<const char *>(buffer.data.data());
	emit textMessageReceived(
		QString::fromUtf8(pData, static_cast<int>(buffer.data.size())));
}

void KWebRtcDataChannel::OnBufferedAmountChange(uint64_t)
{
	const quint64 nBufferedBytes = bufferedAmount();
	const bool bReachedLowWatermark = m_bBackpressured
		&& nBufferedBytes <= m_nLowWatermarkBytes;
	if (m_nHighWatermarkBytes > 0 && nBufferedBytes >= m_nHighWatermarkBytes)
		m_bBackpressured = true;
	else if (bReachedLowWatermark)
		m_bBackpressured = false;

	emit bufferedAmountChanged(nBufferedBytes);
	if (bReachedLowWatermark)
		emit lowWatermarkReached();
}
