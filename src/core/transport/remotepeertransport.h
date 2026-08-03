#ifndef _WINREMOTECONTROL_REMOTEPEERTRANSPORT_H_
#define _WINREMOTECONTROL_REMOTEPEERTRANSPORT_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/inputmessage.h"
#include "core/protocol/clipboardmessage.h"
#include "core/protocol/sessionmessage.h"
#include "core/session/sessionstatemachine.h"

#include <QtCore/QObject>
#include <QtCore/QString>

class KRemotePeerTransport : public QObject
{
	Q_OBJECT

public:
	explicit KRemotePeerTransport(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KRemotePeerTransport() override = default;

	virtual bool initialize(KSessionRole role, QString *pErrorMessage) = 0;
	virtual void shutdown() = 0;
	virtual void createOffer() = 0;
	virtual void restartIce() = 0;
	virtual void handleSignalingMessage(const QString &strMessage) = 0;
	virtual void pushVideoFrame(const KVideoFrame &frame) = 0;
	virtual void sendInputMessage(const KInputMessage &message) = 0;
	virtual void sendClipboardMessage(const KClipboardMessage &message) = 0;
	virtual void sendSessionMessage(const KSessionMessage &message) = 0;
	virtual void setStreamConfig(const KStreamConfig &config) = 0;

signals:
	void signalingMessageReady(const QString &strMessage);
	void stateChanged(const QString &strState);
	void transportError(const QString &strMessage);
	void remoteFrameReady(const KDecodedVideoFrame &frame);
	void remoteFrameStatsReady(int nWidth,
		int nHeight,
		quint64 nFrameIndex,
		qint64 nTimestampMs);
	void networkStatsReady(const KNetworkStats &stats);
	void inputMessageReceived(const KInputMessage &message);
	void inputChannelChanged(bool bOpen);
	void clipboardMessageReceived(const KClipboardMessage &message);
	void clipboardChannelChanged(bool bOpen);
	void sessionMessageReceived(const KSessionMessage &message);
	void sessionChannelChanged(bool bOpen);
	void connectionInterrupted();
	void connectionRestored();
	void connectionTerminated(const QString &strReason);
	void inputBackpressureOverflow();
	void protocolViolation(const QString &strChannel, const QString &strTechnicalMessage);
};

#endif // _WINREMOTECONTROL_REMOTEPEERTRANSPORT_H_
