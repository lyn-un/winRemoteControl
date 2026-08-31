#include "commands/applicationcommandregistry.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QThread>

#include <atomic>
#include <stdexcept>
#include <thread>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	KApplicationCommand SuccessfulCommand(const QString &strId)
	{
		KApplicationCommand command;
		command.strId = strId;
		command.execute = [](const QJsonObject &arguments)
		{
			KApplicationCommandResult result;
			result.status = ApplicationCommandSucceeded;
			result.value = arguments;
			return result;
		};
		return command;
	}

	void TestRegistrationAndExecution()
	{
		KApplicationCommandRegistry registry;
		QString strError;
		Check(!registry.registerCommand(KApplicationCommand(), &strError)
			&& !strError.isEmpty(), QStringLiteral("empty command is rejected"));
		Check(registry.registerCommand(SuccessfulCommand(QStringLiteral("test.echo")), &strError),
			QStringLiteral("valid command is registered"));
		Check(!registry.registerCommand(SuccessfulCommand(QStringLiteral("test.echo")), &strError),
			QStringLiteral("duplicate command is rejected"));
		Check(registry.contains(QStringLiteral("test.echo")),
			QStringLiteral("registered command can be queried"));
		Check(registry.commandIds() == QStringList{
			QStringLiteral("test.echo")},
			QStringLiteral("command ids are exposed in stable sorted order"));
		KApplicationCommand invalidCommand;
		invalidCommand.strId = QStringLiteral("test.invalid_argument");
		invalidCommand.execute = [](const QJsonObject &)
		{
			KApplicationCommandResult result;
			result.status = ApplicationCommandInvalidArgument;
			result.strErrorCode = QStringLiteral("invalid_argument");
			return result;
		};
		Check(registry.registerCommand(invalidCommand, &strError),
			QStringLiteral("invalid-argument command is registered"));

		const QJsonObject arguments{{QStringLiteral("value"), 42}};
		const KApplicationCommandResult success = registry.execute(
			QStringLiteral("test.echo"), arguments);
		Check(success.status == ApplicationCommandSucceeded
			&& success.value.toObject() == arguments,
			QStringLiteral("registered command returns its structured result"));
		const KApplicationCommandResult missing = registry.execute(
			QStringLiteral("test.missing"), QJsonObject());
		Check(missing.status == ApplicationCommandNotFound
			&& missing.strErrorCode == QStringLiteral("unknown_command"),
			QStringLiteral("unknown command returns stable error"));
		const KApplicationCommandResult invalid = registry.execute(
			QStringLiteral("test.invalid_argument"), QJsonObject());
		Check(invalid.status == ApplicationCommandInvalidArgument
			&& invalid.strErrorCode == QStringLiteral("invalid_argument"),
			QStringLiteral("command parameter errors remain structured"));

		KApplicationCommand throwingCommand;
		throwingCommand.strId = QStringLiteral("test.throw");
		throwingCommand.execute = [](const QJsonObject &) -> KApplicationCommandResult
		{
			throw std::runtime_error("simulated failure");
		};
		Check(registry.registerCommand(throwingCommand, &strError),
			QStringLiteral("throwing command registers"));
		const KApplicationCommandResult exception = registry.execute(
			QStringLiteral("test.throw"), QJsonObject());
		Check(exception.status == ApplicationCommandFailed
			&& exception.strErrorCode == QStringLiteral("command_exception"),
			QStringLiteral("command exceptions become structured failures"));
	}

	void TestDisabledAndWrongThread()
	{
		KApplicationCommandRegistry registry;
		KApplicationCommand disabled = SuccessfulCommand(QStringLiteral("test.disabled"));
		disabled.canExecute = []() { return false; };
		Check(registry.registerCommand(disabled, nullptr),
			QStringLiteral("disabled command registers"));
		const KApplicationCommandResult disabledResult = registry.execute(
			QStringLiteral("test.disabled"), QJsonObject());
		Check(disabledResult.status == ApplicationCommandDisabled
			&& disabledResult.strErrorCode == QStringLiteral("command_disabled"),
			QStringLiteral("disabled command is not executed"));

		std::atomic<int> nStatus = -1;
		std::thread worker([&registry, &nStatus]()
		{
			const KApplicationCommandResult result = registry.execute(
				QStringLiteral("test.disabled"), QJsonObject());
			nStatus.store(static_cast<int>(result.status));
		});
		worker.join();
		Check(nStatus.load() == static_cast<int>(ApplicationCommandFailed),
			QStringLiteral("worker thread execution is rejected"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestRegistrationAndExecution();
	TestDisabledAndWrongThread();
	if (g_nFailureCount == 0)
		qInfo() << "All application command registry tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
