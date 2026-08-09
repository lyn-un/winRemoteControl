#ifndef _WINREMOTECONTROL_WEBRTCDATACHANNEL_H_
#define _WINREMOTECONTROL_WEBRTCDATACHANNEL_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <api/data_channel_interface.h>
#include <api/scoped_refptr.h>

#include <atomic>
#include <memory>

class KWebRtcCallbackGate;

class KWebRtcDataChannel final : public QObject, public webrtc::DataChannelObserver
{
	Q_OBJECT

public:
	explicit KWebRtcDataChannel(int nMaximumMessageBytes, QObject *pParent = nullptr);
	~KWebRtcDataChannel() override;

	KWebRtcDataChannel(const KWebRtcDataChannel &) = delete;
	KWebRtcDataChannel &operator=(const KWebRtcDataChannel &) = delete;

	void setChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel);
	void clear();
	bool isOpen() const;
	bool sendText(const QString &strMessage);
	void setBufferWatermarks(quint64 nLowBytes, quint64 nHighBytes);
	quint64 bufferedAmount() const;
	bool isBackpressured() const;

signals:
	void openChanged(bool bOpen);
	void textMessageReceived(const QString &strMessage);
	void messageRejected(int nMessageBytes, const QString &strReason);
	void bufferedAmountChanged(quint64 nBufferedBytes);
	void lowWatermarkReached();

private:
	void OnStateChange() override;
	void OnMessage(const webrtc::DataBuffer &buffer) override;
	void OnBufferedAmountChange(uint64_t nPreviousAmount) override;
	void handleStateChange(quint64 nChannelGeneration);
	void handleMessage(quint64 nChannelGeneration,
		bool bBinary,
		const QByteArray &data);
	void handleBufferedAmountChange(quint64 nChannelGeneration);

	webrtc::scoped_refptr<webrtc::DataChannelInterface> m_spChannel;
	std::shared_ptr<KWebRtcCallbackGate> m_spCallbackGate;
	std::atomic<quint64> m_nChannelGeneration = 0;
	int m_nMaximumMessageBytes = 0;
	quint64 m_nLowWatermarkBytes = 0;
	quint64 m_nHighWatermarkBytes = 0;
	bool m_bBackpressured = false;
};

#endif // _WINREMOTECONTROL_WEBRTCDATACHANNEL_H_
