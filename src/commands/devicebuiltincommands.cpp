#include "commands/extendedbuiltincommands.h"

#include "commands/applicationcommandregistry.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

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

	bool ReadRequiredString(const QJsonObject &arguments,
		const QString &strName,
		QString *pValue)
	{
		if (pValue == nullptr || !arguments.value(strName).isString())
			return false;
		*pValue = arguments.value(strName).toString().trimmed();
		return !pValue->isEmpty();
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

	bool ReadOptionalInteger(const QJsonValue &value, int nFallback, int *pResult)
	{
		if (pResult == nullptr)
			return false;
		if (value.isUndefined())
		{
			*pResult = nFallback;
			return true;
		}
		if (!value.isDouble())
			return false;
		const double number = value.toDouble();
		const int nInteger = value.toInt();
		if (number != static_cast<double>(nInteger))
			return false;
		*pResult = nInteger;
		return true;
	}

	bool RegisterCommand(KApplicationCommandRegistry *pRegistry,
		const QString &strId,
		const std::function<KApplicationCommandResult(const QJsonObject &)> &execute)
	{
		KApplicationCommand command;
		command.strId = strId;
		command.execute = execute;
		return pRegistry->registerCommand(command, nullptr);
	}
}

bool RegisterDeviceBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks)
{
	if (pRegistry == nullptr || !callbacks.startLocalPreview || !callbacks.stopLocalPreview
		|| !callbacks.refreshLanDevices || !callbacks.connectLanDevice
		|| !callbacks.requestRecentDevices || !callbacks.connectRecentDevice
		|| !callbacks.removeRecentDevice || !callbacks.openRecentDeviceTerminal
		|| !callbacks.retryLastConnection
		|| !callbacks.applicationSettings || !callbacks.updateApplicationSettings
		|| !callbacks.updateApplicationTheme || !callbacks.requestTrustedDevices
		|| !callbacks.updateTrustedDevice || !callbacks.revokeTrustedDevice
		|| !callbacks.requestRePairDevice)
	{
		return false;
	}

	if (!RegisterCommand(pRegistry, QStringLiteral("capture.preview.start"),
		[callbacks](const QJsonObject &)
		{
			callbacks.startLocalPreview();
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("capture.preview.stop"),
		[callbacks](const QJsonObject &)
		{
			callbacks.stopLocalPreview();
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("discovery.refresh"),
		[callbacks](const QJsonObject &)
		{
			callbacks.refreshLanDevices();
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("discovery.connect"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strDeviceId;
			if (!ReadRequiredString(arguments, QStringLiteral("deviceId"), &strDeviceId))
				return InvalidArgumentResult(QStringLiteral("deviceId is required"));
			callbacks.connectLanDevice(strDeviceId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("recent.list"),
		[callbacks](const QJsonObject &)
		{
			callbacks.requestRecentDevices();
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("recent.connect"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strDeviceId;
			if (!ReadRequiredString(arguments, QStringLiteral("deviceId"), &strDeviceId))
				return InvalidArgumentResult(QStringLiteral("deviceId is required"));
			callbacks.connectRecentDevice(strDeviceId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("recent.remove"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strDeviceId;
			if (!ReadRequiredString(arguments, QStringLiteral("deviceId"), &strDeviceId))
				return InvalidArgumentResult(QStringLiteral("deviceId is required"));
			callbacks.removeRecentDevice(strDeviceId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("recent.terminal.open"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strDeviceId;
			if (!ReadRequiredString(arguments, QStringLiteral("deviceId"), &strDeviceId))
				return InvalidArgumentResult(QStringLiteral("deviceId is required"));
			callbacks.openRecentDeviceTerminal(strDeviceId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("session.retry"),
		[callbacks](const QJsonObject &)
		{
			callbacks.retryLastConnection();
			return SucceededResult();
		}))
	{
		return false;
	}

	if (!RegisterCommand(pRegistry, QStringLiteral("settings.get"),
		[callbacks](const QJsonObject &)
		{
			const KApplicationSettings settings = callbacks.applicationSettings();
			return SucceededResult(QJsonObject{
				{QStringLiteral("remoteAccessEnabled"), settings.bRemoteAccessEnabled},
				{QStringLiteral("approvalMode"), RemoteApprovalModeName(settings.approvalMode)},
				{QStringLiteral("approvalTimeoutSeconds"), settings.nApprovalTimeoutSeconds},
				{QStringLiteral("defaultListenPort"), settings.nDefaultListenPort},
				{QStringLiteral("themeId"), settings.strThemeId}
			});
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("settings.update"),
		[callbacks](const QJsonObject &arguments)
		{
			const KApplicationSettings current = callbacks.applicationSettings();
			const QJsonValue remoteAccess = arguments.value(QStringLiteral("remoteAccessEnabled"));
			const QJsonValue approvalMode = arguments.value(QStringLiteral("approvalMode"));
			const QJsonValue approvalTimeout = arguments.value(
				QStringLiteral("approvalTimeoutSeconds"));
			const QJsonValue listenPort = arguments.value(QStringLiteral("defaultListenPort"));
			if ((!remoteAccess.isUndefined() && !remoteAccess.isBool())
				|| (!approvalMode.isUndefined() && !approvalMode.isString())
				|| (!approvalTimeout.isUndefined() && !approvalTimeout.isDouble())
				|| (!listenPort.isUndefined() && !listenPort.isDouble()))
			{
				return InvalidArgumentResult(QStringLiteral("Application settings are invalid"));
			}
			const bool bRemoteAccessEnabled = remoteAccess.isUndefined()
				? current.bRemoteAccessEnabled : remoteAccess.toBool();
			const QString strApprovalMode = approvalMode.isUndefined()
				? RemoteApprovalModeName(current.approvalMode) : approvalMode.toString();
			int nApprovalTimeoutSeconds = 0;
			int nDefaultListenPort = 0;
			KRemoteApprovalMode parsedMode;
			if (!ReadOptionalInteger(approvalTimeout,
					current.nApprovalTimeoutSeconds, &nApprovalTimeoutSeconds)
				|| !ReadOptionalInteger(listenPort,
					current.nDefaultListenPort, &nDefaultListenPort)
				|| !RemoteApprovalModeFromName(strApprovalMode, &parsedMode)
				|| nApprovalTimeoutSeconds < 10 || nApprovalTimeoutSeconds > 120
				|| nDefaultListenPort <= 0 || nDefaultListenPort > 65535)
			{
				return InvalidArgumentResult(QStringLiteral("Application settings are invalid"));
			}
			callbacks.updateApplicationSettings(bRemoteAccessEnabled,
				strApprovalMode, nApprovalTimeoutSeconds, nDefaultListenPort);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("settings.theme.set"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strThemeId;
			if (!ReadRequiredString(arguments, QStringLiteral("themeId"), &strThemeId)
				|| !IsApplicationThemeIdValid(strThemeId))
			{
				return InvalidArgumentResult(QStringLiteral("themeId is invalid"));
			}
			callbacks.updateApplicationTheme(strThemeId);
			return SucceededResult();
		}))
	{
		return false;
	}

	return RegisterCommand(pRegistry, QStringLiteral("trusted.list"),
		[callbacks](const QJsonObject &)
		{
			callbacks.requestTrustedDevices();
			return SucceededResult();
		})
		&& RegisterCommand(pRegistry, QStringLiteral("trusted.update"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strDeviceId;
			KPermissionScopes permissions;
			const QJsonValue alias = arguments.value(QStringLiteral("alias"));
			if (!ReadRequiredString(arguments, QStringLiteral("deviceId"), &strDeviceId)
				|| (!alias.isUndefined() && !alias.isString())
				|| !ReadPermissions(arguments.value(QStringLiteral("permissions")), &permissions))
			{
				return InvalidArgumentResult(
					QStringLiteral("deviceId, alias and permissions are required"));
			}
			callbacks.updateTrustedDevice(strDeviceId, alias.toString(), permissions);
			return SucceededResult();
		})
		&& RegisterCommand(pRegistry, QStringLiteral("trusted.revoke"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strDeviceId;
			if (!ReadRequiredString(arguments, QStringLiteral("deviceId"), &strDeviceId))
				return InvalidArgumentResult(QStringLiteral("deviceId is required"));
			callbacks.revokeTrustedDevice(strDeviceId);
			return SucceededResult();
		})
		&& RegisterCommand(pRegistry, QStringLiteral("trusted.repair"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strDeviceId;
			if (!ReadRequiredString(arguments, QStringLiteral("deviceId"), &strDeviceId))
				return InvalidArgumentResult(QStringLiteral("deviceId is required"));
			callbacks.requestRePairDevice(strDeviceId);
			return SucceededResult();
		});
}
