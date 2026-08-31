#include "commands/applicationcommandregistry.h"

#include <QtCore/QThread>

#include <exception>

KApplicationCommandRegistry::KApplicationCommandRegistry(QObject *pParent)
	: QObject(pParent)
{
}

KApplicationCommandRegistry::~KApplicationCommandRegistry()
{
}

bool KApplicationCommandRegistry::registerCommand(const KApplicationCommand &command,
	QString *pError)
{
	if (!isApplicationThread())
	{
		if (pError != nullptr)
			*pError = QStringLiteral("Commands must be registered on the application thread");
		return false;
	}
	if (command.strId.trimmed().isEmpty())
	{
		if (pError != nullptr)
			*pError = QStringLiteral("Command ID must not be empty");
		return false;
	}
	if (!command.execute)
	{
		if (pError != nullptr)
			*pError = QStringLiteral("Command execute callback must not be empty");
		return false;
	}
	if (m_commands.contains(command.strId))
	{
		if (pError != nullptr)
			*pError = QStringLiteral("Command is already registered: %1").arg(command.strId);
		return false;
	}

	m_commands.insert(command.strId, command);
	if (pError != nullptr)
		pError->clear();
	return true;
}

bool KApplicationCommandRegistry::contains(const QString &strCommandId) const
{
	return m_commands.contains(strCommandId);
}

QStringList KApplicationCommandRegistry::commandIds() const
{
	QStringList commandIdList = m_commands.keys();
	commandIdList.sort(Qt::CaseSensitive);
	return commandIdList;
}

KApplicationCommandResult KApplicationCommandRegistry::execute(
	const QString &strCommandId,
	const QJsonObject &arguments) const
{
	if (!isApplicationThread())
	{
		return failedResult(ApplicationCommandFailed,
			QStringLiteral("wrong_thread"),
			QStringLiteral("Application commands must execute on the application thread"));
	}

	const auto iter = m_commands.constFind(strCommandId);
	if (iter == m_commands.constEnd())
	{
		return failedResult(ApplicationCommandNotFound,
			QStringLiteral("unknown_command"),
			QStringLiteral("Unknown application command: %1").arg(strCommandId));
	}
	try
	{
		if (iter->canExecute && !iter->canExecute())
		{
			return failedResult(ApplicationCommandDisabled,
				QStringLiteral("command_disabled"),
				QStringLiteral("Command is unavailable in the current application state"));
		}

		return iter->execute(arguments);
	}
	catch (const std::exception &error)
	{
		return failedResult(ApplicationCommandFailed,
			QStringLiteral("command_exception"),
			QString::fromUtf8(error.what()));
	}
	catch (...)
	{
		return failedResult(ApplicationCommandFailed,
			QStringLiteral("command_exception"),
			QStringLiteral("Application command raised an unknown exception"));
	}
}

bool KApplicationCommandRegistry::isApplicationThread() const
{
	return QThread::currentThread() == thread();
}

KApplicationCommandResult KApplicationCommandRegistry::failedResult(
	KApplicationCommandStatus status,
	const QString &strErrorCode,
	const QString &strTechnicalMessage) const
{
	KApplicationCommandResult result;
	result.status = status;
	result.strErrorCode = strErrorCode;
	result.strTechnicalMessage = strTechnicalMessage;
	return result;
}
