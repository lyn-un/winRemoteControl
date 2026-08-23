#ifndef _WINREMOTECONTROL_REMOTEPEERTRANSPORT_H_
#define _WINREMOTECONTROL_REMOTEPEERTRANSPORT_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/inputmessage.h"
#include "core/protocol/clipboardmessage.h"
#include "core/protocol/filetransfercontrolmessage.h"
#include "core/protocol/filetransferlifecyclemessage.h"
#include "core/protocol/sessionmessage.h"
#include "core/protocol/terminalmessage.h"
#include "core/protocol/webrtcsignalingmessage.h"
#include "core/session/sessionstatemachine.h"
#include "core/transport/peerinitializationresult.h"

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

	virtual KPeerInitializationResult initialize(KSessionRole role,
		quint64 nGeneration) = 0;
	virtual void requestShutdown(quint64 nGeneration) = 0;
	virtual quint64 generation() const = 0;
	virtual void createOffer() = 0;
	virtual void restartIce() = 0;
	virtual void handleSignalingMessage(const KWebRtcSignalingMessage &message) = 0;
	virtual void pushVideoFrame(const KVideoFrame &frame) = 0;
	virtual void sendInputMessage(const KInputMessage &message) = 0;
	virtual void sendClipboardMessage(const KClipboardMessage &message) = 0;
	enum KSessionMessageSendStatus
	{
		SessionMessageAccepted,
		SessionMessageChannelUnavailable,
		SessionMessageQueueOverflow,
		SessionMessageTransportFailed
	};

	virtual KSessionMessageSendStatus sendTerminalControlMessage(
		const KTerminalMessage &message) = 0;
	virtual bool sendTerminalData(const QByteArray &data) = 0;
	virtual bool terminalBackpressured() const = 0;
	virtual bool ensureFileTransferChannels() { return false; }
	virtual KSessionMessageSendStatus sendFileTransferLifecycleMessage(
		const KFileTransferLifecycleMessage &) { return SessionMessageChannelUnavailable; }
	virtual bool sendFileTransferControlMessage(
		const KFileTransferControlMessage &) { return false; }
	virtual bool sendFileTransferData(const QByteArray &) { return false; }
	virtual bool fileTransferBackpressured() const { return false; }
	virtual KSessionMessageSendStatus sendSessionMessage(
		const KSessionMessage &message) = 0;
	virtual void setInputRealtimeEnabled(bool bEnabled) = 0;
	virtual void setStreamConfig(const KStreamConfig &config) = 0;

signals:
	void shutdownFinished(quint64 nGeneration);
	void signalingMessageReady(quint64 nGeneration, const QString &strMessage);
	void stateChanged(quint64 nGeneration, const QString &strState);
	void transportError(quint64 nGeneration, const QString &strMessage);
	void remoteFrameReady(quint64 nGeneration, const KDecodedVideoFrame &frame);
	void remoteFrameStatsReady(quint64 nGeneration, int nWidth,
		int nHeight,
		quint64 nFrameIndex,
		qint64 nTimestampMs);
	void networkStatsReady(quint64 nGeneration, const KNetworkStats &stats);
	void inputMessageReceived(quint64 nGeneration, const KInputMessage &message);
	void inputChannelChanged(quint64 nGeneration, bool bOpen);
	void clipboardMessageReceived(quint64 nGeneration, const KClipboardMessage &message);
	void clipboardChannelChanged(quint64 nGeneration, bool bOpen);
	void terminalControlMessageReceived(quint64 nGeneration, const KTerminalMessage &message);
	void terminalDataReceived(quint64 nGeneration, const QByteArray &data);
	void terminalChannelChanged(quint64 nGeneration, bool bOpen);
	void terminalLowWatermarkReached(quint64 nGeneration);
	void fileTransferLifecycleMessageReceived(quint64 nGeneration,
		const KFileTransferLifecycleMessage &message);
	void fileTransferControlMessageReceived(quint64 nGeneration,
		const KFileTransferControlMessage &message);
	void fileTransferDataReceived(quint64 nGeneration, const QByteArray &data);
	void fileTransferChannelsChanged(quint64 nGeneration,
		bool bControlOpen,
		bool bDataOpen);
	void fileTransferLowWatermarkReached(quint64 nGeneration);
	void sessionMessageReceived(quint64 nGeneration, const KSessionMessage &message);
	void sessionChannelChanged(quint64 nGeneration, bool bOpen);
	void connectionInterrupted(quint64 nGeneration);
	void connectionRestored(quint64 nGeneration);
	void connectionTerminated(quint64 nGeneration, const QString &strReason);
	void inputBackpressureOverflow(quint64 nGeneration);
	void protocolViolation(quint64 nGeneration, const QString &strChannel, const QString &strTechnicalMessage);
};

#endif // _WINREMOTECONTROL_REMOTEPEERTRANSPORT_H_
