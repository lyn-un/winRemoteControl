#include "transport/webrtc/webrtcdatachannel.h"

#include "transport/webrtc/webrtccallbackgate.h"

#include <QtCore/QByteArray>
#include <QtCore/QThread>

#include <string>
#include <utility>

KWebRtcDataChannel::KWebRtcDataChannel(int nMaximumMessageBytes, QObject *pParent)
	: QObject(pParent)
	, m_spCallbackGate(std::make_shared<KWebRtcCallbackGate>())
	, m_nMaximumMessageBytes(nMaximumMessageBytes)
{
}

KWebRtcDataChannel::~KWebRtcDataChannel()
{
	m_spCallbackGate->close();
	clear();
}

void KWebRtcDataChannel::setChannel(
	webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel)
{
	Q_ASSERT(QThread::currentThread() == thread());
	clear();
	m_spChannel = std::move(spChannel);
	if (m_spChannel == nullptr)
		return;

	const quint64 nChannelGeneration = m_nChannelGeneration.fetch_add(1) + 1;
	m_spCallbackGate->open(this, nChannelGeneration);
	m_spChannel->RegisterObserver(this);
	emit openChanged(isOpen());
}

void KWebRtcDataChannel::clear()
{
	Q_ASSERT(QThread::currentThread() == thread());
	m_spCallbackGate->close();
	m_nChannelGeneration.fetch_add(1);
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
	Q_ASSERT(QThread::currentThread() == thread());
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

bool KWebRtcDataChannel::sendBinary(const QByteArray &data)
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (!isOpen() || data.isEmpty() || data.size() > m_nMaximumMessageBytes)
		return false;
	webrtc::DataBuffer buffer(webrtc::CopyOnWriteBuffer(
		reinterpret_cast<const uint8_t *>(data.constData()),
		static_cast<size_t>(data.size())), true);
	const bool bSent = m_spChannel->Send(buffer);
	const quint64 nBufferedBytes = bufferedAmount();
	if (m_nHighWatermarkBytes > 0 && nBufferedBytes >= m_nHighWatermarkBytes)
		m_bBackpressured = true;
	emit bufferedAmountChanged(nBufferedBytes);
	return bSent;
}

void KWebRtcDataChannel::setBufferWatermarks(quint64 nLowBytes, quint64 nHighBytes)
{
	Q_ASSERT(QThread::currentThread() == thread());
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
	const quint64 nChannelGeneration = m_nChannelGeneration.load();
	m_spCallbackGate->post(nChannelGeneration,
		[nChannelGeneration](QObject *pTarget)
		{
			static_cast<KWebRtcDataChannel *>(pTarget)
				->handleStateChange(nChannelGeneration);
		});
}

void KWebRtcDataChannel::OnMessage(const webrtc::DataBuffer &buffer)
{
	const quint64 nChannelGeneration = m_nChannelGeneration.load();
	const QByteArray data(reinterpret_cast<const char *>(buffer.data.data()),
		static_cast<qsizetype>(buffer.data.size()));
	m_spCallbackGate->post(nChannelGeneration,
		[nChannelGeneration, bBinary = buffer.binary, data](QObject *pTarget)
		{
			static_cast<KWebRtcDataChannel *>(pTarget)
				->handleMessage(nChannelGeneration, bBinary, data);
		});
}

void KWebRtcDataChannel::OnBufferedAmountChange(uint64_t)
{
	const quint64 nChannelGeneration = m_nChannelGeneration.load();
	m_spCallbackGate->post(nChannelGeneration,
		[nChannelGeneration](QObject *pTarget)
		{
			static_cast<KWebRtcDataChannel *>(pTarget)
				->handleBufferedAmountChange(nChannelGeneration);
		});
}

void KWebRtcDataChannel::handleStateChange(quint64 nChannelGeneration)
{
	if (nChannelGeneration != m_nChannelGeneration.load())
		return;
	emit openChanged(isOpen());
}

void KWebRtcDataChannel::handleMessage(quint64 nChannelGeneration,
	bool bBinary,
	const QByteArray &data)
{
	if (nChannelGeneration != m_nChannelGeneration.load())
		return;
	if (bBinary)
	{
		if (data.size() > m_nMaximumMessageBytes)
		{
			emit messageRejected(data.size(),
				QStringLiteral("DataChannel message is too large"));
			return;
		}
		emit binaryMessageReceived(data);
		return;
	}
	if (data.size() > m_nMaximumMessageBytes)
	{
		emit messageRejected(data.size(),
			QStringLiteral("DataChannel message is too large"));
		return;
	}
	emit textMessageReceived(QString::fromUtf8(data));
}

void KWebRtcDataChannel::handleBufferedAmountChange(quint64 nChannelGeneration)
{
	if (nChannelGeneration != m_nChannelGeneration.load())
		return;
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
