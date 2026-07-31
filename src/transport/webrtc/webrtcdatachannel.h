#ifndef _WINREMOTECONTROL_WEBRTCDATACHANNEL_H_
#define _WINREMOTECONTROL_WEBRTCDATACHANNEL_H_

#include <QtCore/QObject>
#include <QtCore/QString>

#include <api/data_channel_interface.h>
#include <api/scoped_refptr.h>

class KWebRtcDataChannel final : public QObject, public webrtc::DataChannelObserver
{
	Q_OBJECT

public:
	explicit KWebRtcDataChannel(QObject *pParent = nullptr);
	~KWebRtcDataChannel() override;

	KWebRtcDataChannel(const KWebRtcDataChannel &) = delete;
	KWebRtcDataChannel &operator=(const KWebRtcDataChannel &) = delete;

	void setChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel);
	void clear();
	bool isOpen() const;
	bool sendText(const QString &strMessage);

signals:
	void openChanged(bool bOpen);
	void textMessageReceived(const QString &strMessage);

private:
	void OnStateChange() override;
	void OnMessage(const webrtc::DataBuffer &buffer) override;
	void OnBufferedAmountChange(uint64_t nPreviousAmount) override;

	webrtc::scoped_refptr<webrtc::DataChannelInterface> m_spChannel;
};

#endif // _WINREMOTECONTROL_WEBRTCDATACHANNEL_H_
