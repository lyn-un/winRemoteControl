#include "core/transport/peerinitializationresult.h"

bool KPeerInitializationResult::succeeded() const
{
	return status == SucceededPeerInitializationStatus;
}

KPeerInitializationResult KPeerInitializationResult::success()
{
	KPeerInitializationResult result;
	result.status = SucceededPeerInitializationStatus;
	return result;
}

KPeerInitializationResult KPeerInitializationResult::rollbackPending(
	KPeerInitializationStage stage, const QString &strTechnicalMessage)
{
	KPeerInitializationResult result;
	result.status = RollbackPendingPeerInitializationStatus;
	result.stage = stage;
	result.strTechnicalMessage = strTechnicalMessage;
	return result;
}

KPeerInitializationResult KPeerInitializationResult::rejected(
	const QString &strTechnicalMessage)
{
	KPeerInitializationResult result;
	result.status = RejectedPeerInitializationStatus;
	result.strTechnicalMessage = strTechnicalMessage;
	return result;
}

QString KPeerInitializationResult::stageName(KPeerInitializationStage stage)
{
	switch (stage)
	{
	case ThreadsPeerInitializationStage:
		return QStringLiteral("Threads");
	case FactoryPeerInitializationStage:
		return QStringLiteral("Factory");
	case PeerConnectionPeerInitializationStage:
		return QStringLiteral("PeerConnection");
	case InputChannelPeerInitializationStage:
		return QStringLiteral("InputChannel");
	case RealtimeInputChannelPeerInitializationStage:
		return QStringLiteral("RealtimeInputChannel");
	case SessionChannelPeerInitializationStage:
		return QStringLiteral("SessionChannel");
	case ClipboardChannelPeerInitializationStage:
		return QStringLiteral("ClipboardChannel");
	case LocalVideoTrackPeerInitializationStage:
		return QStringLiteral("LocalVideoTrack");
	case RemoteVideoReceiverPeerInitializationStage:
		return QStringLiteral("RemoteVideoReceiver");
	case NoPeerInitializationStage:
	default:
		return QStringLiteral("None");
	}
}
