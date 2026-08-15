#include "core/security/permissionscope.h"

QStringList PermissionScopeNames(KPermissionScopes permissions)
{
	QStringList names;
	if (permissions.testFlag(ViewScreenPermissionScope))
		names.append(QStringLiteral("viewScreen"));
	if (permissions.testFlag(InputControlPermissionScope))
		names.append(QStringLiteral("inputControl"));
	if (permissions.testFlag(ClipboardPermissionScope))
		names.append(QStringLiteral("clipboard"));
	if (permissions.testFlag(TerminalPermissionScope))
		names.append(QStringLiteral("terminal"));
	return names;
}

bool PermissionScopesFromNames(const QStringList &names,
	KPermissionScopes *pPermissions)
{
	if (pPermissions == nullptr)
		return false;

	KPermissionScopes permissions;
	for (const QString &strName : names)
	{
		if (strName == QStringLiteral("viewScreen"))
			permissions |= ViewScreenPermissionScope;
		else if (strName == QStringLiteral("inputControl"))
			permissions |= InputControlPermissionScope;
		else if (strName == QStringLiteral("clipboard"))
			permissions |= ClipboardPermissionScope;
		else if (strName == QStringLiteral("terminal"))
			permissions |= TerminalPermissionScope;
		else
			return false;
	}
	*pPermissions = permissions;
	return true;
}
