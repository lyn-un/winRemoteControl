#include "commands/builtincommands.h"

#include "commands/applicationcommandregistry.h"
#include "core/session/sessionstatemachine.h"
#include "core/security/permissionscope.h"
#include "session/sessioncontroller.h"
#include "session/sessionviewmodel.h"
#include "settings/devicesecuritypreferenceservice.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>

namespace
{
	KApplicationCommandResult SucceededResult(const QJsonValue &value = QJsonValue())
	{
		KApplicationCommandResult result;
		result.status = ApplicationCommandSucceeded;
		result.value = value;
		return result;
	}

	KApplicationCommandResult InvalidArgumentResult(const QString &strMessage)
	{
		KApplicationCommandResult result;
		result.status = ApplicationCommandInvalidArgument;
		result.strErrorCode = QStringLiteral("invalid_argument");
		result.strTechnicalMessage = strMessage;
		return result;
	}

	bool ReadPort(const QJsonObject &arguments, quint16 *pPort)
	{
		if (pPort == nullptr)
			return false;
		const QJsonValue value = arguments.value(QStringLiteral("port"));
		if (!value.isDouble())
			return false;
		const double nPortValue = value.toDouble(-1.0);
		if (nPortValue < 0.0 || nPortValue > 65535.0
			|| nPortValue != static_cast<double>(static_cast<quint16>(nPortValue)))
		{
			return false;
		}
		*pPort = static_cast<quint16>(nPortValue);
		return true;
	}

	bool ReadPermissions(const QJsonValue &value, KPermissionScopes *pPermissions)
	{
		if (pPermissions == nullptr || !value.isArray())
			return false;
		QStringList names;
		for (const QJsonValue &item : value.toArray())
		{
			if (!item.isString())
				return false;
			names.append(item.toString());
		}
		return PermissionScopesFromNames(names, pPermissions);
	}
}

bool RegisterBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	KSessionViewModel *pSessionViewModel,
	KSessionController *pSessionController,
	KDeviceSecurityPreferenceService *pSecurityPreferenceService)
{
	if (pRegistry == nullptr || pSessionViewModel == nullptr || pSessionController == nullptr
		|| pSecurityPreferenceService == nullptr)
		return false;

	QString strError;
	KApplicationCommand setRoleCommand;
	setRoleCommand.strId = QStringLiteral("application.set_role");
	setRoleCommand.canExecute = [pSessionController]() { return pSessionController->isIdle(); };
	setRoleCommand.execute = [pSessionViewModel](const QJsonObject &arguments)
	{
		const QString strRole = arguments.value(QStringLiteral("role")).toString();
		KSessionRole role = ControllerSessionRole;
		if (!KSessionStateMachine::roleFromString(strRole, &role))
			return InvalidArgumentResult(QStringLiteral("role must be controller or controlled"));
		pSessionViewModel->setRole(strRole);
		return SucceededResult(QJsonObject{{QStringLiteral("role"), strRole}});
	};
	if (!pRegistry->registerCommand(setRoleCommand, &strError))
		return false;

	KApplicationCommand startServerCommand;
	startServerCommand.strId = QStringLiteral("signaling.start_server");
	startServerCommand.canExecute = [pSessionController]()
	{
		return pSessionController->isIdle()
			&& pSessionController->sessionRole() == ControlledSessionRole;
	};
	startServerCommand.execute = [pSessionViewModel](const QJsonObject &arguments)
	{
		quint16 nPort = 0;
		if (!ReadPort(arguments, &nPort))
			return InvalidArgumentResult(QStringLiteral("port must be an integer from 0 to 65535"));
		pSessionViewModel->startSignalingServer(nPort);
		return SucceededResult(QJsonObject{{QStringLiteral("requestedPort"), nPort}});
	};
	if (!pRegistry->registerCommand(startServerCommand, &strError))
		return false;

	KApplicationCommand disconnectCommand;
	disconnectCommand.strId = QStringLiteral("session.disconnect");
	disconnectCommand.canExecute = [pSessionViewModel]()
	{
		return pSessionViewModel->sessionState() != IdleSessionState;
	};
	disconnectCommand.execute = [pSessionViewModel](const QJsonObject &)
	{
		pSessionViewModel->disconnectSession();
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(disconnectCommand, &strError))
		return false;

	KApplicationCommand connectCommand;
	connectCommand.strId = QStringLiteral("session.connect");
	connectCommand.canExecute = [pSessionController, pSessionViewModel]()
	{
		return pSessionViewModel->sessionState() == IdleSessionState
			&& pSessionController->sessionRole() == ControllerSessionRole;
	};
	connectCommand.execute = [pSessionViewModel](const QJsonObject &arguments)
	{
		const QString strHost = arguments.value(QStringLiteral("host")).toString().trimmed();
		quint16 nPort = 0;
		if (strHost.isEmpty() || !ReadPort(arguments, &nPort) || nPort == 0)
			return InvalidArgumentResult(QStringLiteral("host and non-zero port are required"));
		pSessionViewModel->connectSignaling(strHost, nPort);
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(connectCommand, &strError))
		return false;

	KApplicationCommand accessCommand;
	accessCommand.strId = QStringLiteral("access.respond");
	accessCommand.canExecute = [pSessionController, pSessionViewModel]()
	{
		return pSessionController->sessionRole() == ControlledSessionRole
			&& pSessionViewModel->sessionState() == AwaitingApprovalSessionState;
	};
	accessCommand.execute = [pSessionController](const QJsonObject &arguments)
	{
		const QString strRequestId = arguments.value(QStringLiteral("requestId")).toString();
		const QJsonValue accepted = arguments.value(QStringLiteral("accepted"));
		if (strRequestId.isEmpty() || !accepted.isBool())
			return InvalidArgumentResult(QStringLiteral("requestId and accepted are required"));
		pSessionController->respondIncomingAccessRequest(strRequestId, accepted.toBool());
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(accessCommand, &strError))
		return false;

	KApplicationCommand pairingCommand;
	pairingCommand.strId = QStringLiteral("pairing.respond");
	pairingCommand.canExecute = [pSessionViewModel]()
	{
		return pSessionViewModel->sessionState() == PairingSessionState;
	};
	pairingCommand.execute = [pSessionController](const QJsonObject &arguments)
	{
		const QString strRequestId = arguments.value(QStringLiteral("requestId")).toString();
		const QJsonValue accepted = arguments.value(QStringLiteral("accepted"));
		KPermissionScopes permissions;
		if (strRequestId.isEmpty() || !accepted.isBool())
		{
			return InvalidArgumentResult(
				QStringLiteral("requestId and accepted are required"));
		}
		if (accepted.toBool()
			&& !ReadPermissions(arguments.value(QStringLiteral("permissions")), &permissions))
			return InvalidArgumentResult(QStringLiteral("Accepted pairing requires permissions"));
		pSessionController->respondPairingRequest(strRequestId, accepted.toBool(), permissions);
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(pairingCommand, &strError))
		return false;

	KApplicationCommand streamStartCommand;
	streamStartCommand.strId = QStringLiteral("stream.start");
	streamStartCommand.canExecute = [pSessionController, pSessionViewModel]()
	{
		return pSessionController->sessionRole() == ControllerSessionRole
			&& pSessionViewModel->sessionState() == ConnectedSessionState;
	};
	streamStartCommand.execute = [pSessionViewModel](const QJsonObject &)
	{
		pSessionViewModel->enterRemoteDesktop();
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(streamStartCommand, &strError))
		return false;

	KApplicationCommand streamStopCommand;
	streamStopCommand.strId = QStringLiteral("stream.stop");
	streamStopCommand.canExecute = [pSessionController, pSessionViewModel]()
	{
		return pSessionController->sessionRole() == ControllerSessionRole
			&& pSessionViewModel->sessionState() == StreamingSessionState;
	};
	streamStopCommand.execute = [pSessionViewModel](const QJsonObject &)
	{
		pSessionViewModel->leaveRemoteDesktop();
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(streamStopCommand, &strError))
		return false;

	KApplicationCommand privacyCommand;
	privacyCommand.strId = QStringLiteral("security.privacy.set");
	privacyCommand.canExecute = [pSessionController, pSessionViewModel,
		pSecurityPreferenceService]()
	{
		const KSessionState state = pSessionViewModel->sessionState();
		return pSessionController->sessionRole() == ControllerSessionRole
			&& (state == StreamingSessionState || state == ReconnectingSessionState)
			&& !pSecurityPreferenceService->isPrivacyCommandPending();
	};
	privacyCommand.execute = [pSecurityPreferenceService](const QJsonObject &arguments)
	{
		const QString strMode = arguments.value(QStringLiteral("mode")).toString();
		KPrivacyMode mode = UnknownPrivacyMode;
		if (strMode == QStringLiteral("disabled"))
			mode = DisabledPrivacyMode;
		else if (strMode == QStringLiteral("privacyOverlay"))
			mode = PrivacyOverlayPrivacyMode;
		else if (strMode == QStringLiteral("displayOff"))
			mode = DisplayOffPrivacyMode;
		if (mode == UnknownPrivacyMode)
			return InvalidArgumentResult(QStringLiteral("Unknown privacy mode"));
		pSecurityPreferenceService->requestPrivacyMode(mode);
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(privacyCommand, &strError))
		return false;

	KApplicationCommand postSessionCommand;
	postSessionCommand.strId = QStringLiteral("security.post_session.set");
	postSessionCommand.canExecute = [pSessionController, pSessionViewModel,
		pSecurityPreferenceService]()
	{
		const KSessionState state = pSessionViewModel->sessionState();
		return pSessionController->sessionRole() == ControllerSessionRole
			&& (state == StreamingSessionState || state == ReconnectingSessionState)
			&& !pSecurityPreferenceService->isPostSessionActionCommandPending();
	};
	postSessionCommand.execute = [pSecurityPreferenceService](const QJsonObject &arguments)
	{
		const QString strAction = arguments.value(QStringLiteral("action")).toString();
		KPostSessionAction action = UnknownPostSessionAction;
		if (strAction == QStringLiteral("none"))
			action = NoPostSessionAction;
		else if (strAction == QStringLiteral("lockWorkstation"))
			action = LockWorkstationPostSessionAction;
		if (action == UnknownPostSessionAction)
			return InvalidArgumentResult(QStringLiteral("Unknown post-session action"));
		pSecurityPreferenceService->requestPostSessionAction(action);
		return SucceededResult();
	};
	if (!pRegistry->registerCommand(postSessionCommand, &strError))
		return false;

	KApplicationCommand qualityCommand;
	qualityCommand.strId = QStringLiteral("desktop.quality.set");
	qualityCommand.canExecute = [pSessionController, pSessionViewModel]()
	{
		return pSessionController->sessionRole() == ControllerSessionRole
			&& pSessionViewModel->sessionState() == StreamingSessionState;
	};
	qualityCommand.execute = [pSessionViewModel](const QJsonObject &arguments)
	{
		KStreamConfig config;
		config.nFps = arguments.value(QStringLiteral("fps")).toInt(config.nFps);
		config.nWidth = arguments.value(QStringLiteral("width")).toInt(config.nWidth);
		config.nHeight = arguments.value(QStringLiteral("height")).toInt(config.nHeight);
		config.nBitrateKbps = arguments.value(QStringLiteral("bitrateKbps"))
			.toInt(config.nBitrateKbps);
		if (config.nFps < 1 || config.nFps > 240
			|| config.nWidth < 320 || config.nWidth > 7680
			|| config.nHeight < 240 || config.nHeight > 4320
			|| config.nBitrateKbps < 128 || config.nBitrateKbps > 200000)
		{
			return InvalidArgumentResult(QStringLiteral("Stream configuration is out of range"));
		}
		pSessionViewModel->sendStreamConfig(config);
		return SucceededResult();
	};
	return pRegistry->registerCommand(qualityCommand, &strError);
}
