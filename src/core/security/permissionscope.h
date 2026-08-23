#ifndef _WINREMOTECONTROL_CORE_SECURITY_PERMISSIONSCOPE_H_
#define _WINREMOTECONTROL_CORE_SECURITY_PERMISSIONSCOPE_H_

#include <QtCore/QFlags>
#include <QtCore/QStringList>

enum KPermissionScope
{
	NoPermissionScope = 0x00,
	ViewScreenPermissionScope = 0x01,
	InputControlPermissionScope = 0x02,
	ClipboardPermissionScope = 0x04,
	TerminalPermissionScope = 0x08,
	FileTransferPermissionScope = 0x10
};

Q_DECLARE_FLAGS(KPermissionScopes, KPermissionScope)
Q_DECLARE_OPERATORS_FOR_FLAGS(KPermissionScopes)

constexpr quint32 kAllPermissionScopeBits =
	ViewScreenPermissionScope
	| InputControlPermissionScope
	| ClipboardPermissionScope
	| TerminalPermissionScope
	| FileTransferPermissionScope;

QStringList PermissionScopeNames(KPermissionScopes permissions);
bool PermissionScopesFromNames(const QStringList &names,
	KPermissionScopes *pPermissions);

#endif // _WINREMOTECONTROL_CORE_SECURITY_PERMISSIONSCOPE_H_
