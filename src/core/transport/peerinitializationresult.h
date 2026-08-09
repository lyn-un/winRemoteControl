#ifndef _WINREMOTECONTROL_PEERINITIALIZATIONRESULT_H_
#define _WINREMOTECONTROL_PEERINITIALIZATIONRESULT_H_

#include <QtCore/QString>

enum KPeerInitializationStatus
{
	SucceededPeerInitializationStatus,
	RollbackPendingPeerInitializationStatus,
	RejectedPeerInitializationStatus
};

enum KPeerInitializationStage
{
	NoPeerInitializationStage,
	ThreadsPeerInitializationStage,
	FactoryPeerInitializationStage,
	PeerConnectionPeerInitializationStage,
	InputChannelPeerInitializationStage,
	RealtimeInputChannelPeerInitializationStage,
	SessionChannelPeerInitializationStage,
	ClipboardChannelPeerInitializationStage,
	LocalVideoTrackPeerInitializationStage,
	RemoteVideoReceiverPeerInitializationStage
};

struct KPeerInitializationResult
{
	KPeerInitializationStatus status = RejectedPeerInitializationStatus;
	KPeerInitializationStage stage = NoPeerInitializationStage;
	QString strTechnicalMessage;

	bool succeeded() const;
	static KPeerInitializationResult success();
	static KPeerInitializationResult rollbackPending(
		KPeerInitializationStage stage, const QString &strTechnicalMessage);
	static KPeerInitializationResult rejected(const QString &strTechnicalMessage);
	static QString stageName(KPeerInitializationStage stage);
};

#endif // _WINREMOTECONTROL_PEERINITIALIZATIONRESULT_H_
