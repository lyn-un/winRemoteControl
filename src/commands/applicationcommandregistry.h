#ifndef _WINREMOTECONTROL_APPLICATIONCOMMANDREGISTRY_H_
#define _WINREMOTECONTROL_APPLICATIONCOMMANDREGISTRY_H_

#include "core/commands/applicationcommand.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QStringList>

class KApplicationCommandRegistry : public QObject
{
public:
	explicit KApplicationCommandRegistry(QObject *pParent = nullptr);
	~KApplicationCommandRegistry() override;

	KApplicationCommandRegistry(const KApplicationCommandRegistry &) = delete;
	KApplicationCommandRegistry &operator=(const KApplicationCommandRegistry &) = delete;

	bool registerCommand(const KApplicationCommand &command, QString *pError);
	bool contains(const QString &strCommandId) const;
	QStringList commandIds() const;
	KApplicationCommandResult execute(const QString &strCommandId,
		const QJsonObject &arguments) const;

private:
	bool isApplicationThread() const;
	KApplicationCommandResult failedResult(KApplicationCommandStatus status,
		const QString &strErrorCode,
		const QString &strTechnicalMessage) const;

	QHash<QString, KApplicationCommand> m_commands;
};

#endif // _WINREMOTECONTROL_APPLICATIONCOMMANDREGISTRY_H_
