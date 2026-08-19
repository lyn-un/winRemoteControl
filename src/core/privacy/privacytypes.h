#ifndef _WINREMOTECONTROL_CORE_PRIVACY_PRIVACYTYPES_H_
#define _WINREMOTECONTROL_CORE_PRIVACY_PRIVACYTYPES_H_

#include <QtCore/QString>

enum KPrivacyMode
{
	UnknownPrivacyMode = -1,
	DisabledPrivacyMode,
	PrivacyOverlayPrivacyMode,
	DisplayOffPrivacyMode
};

enum KPrivacyModeState
{
	InactivePrivacyModeState,
	ApplyingPrivacyModeState,
	ActivePrivacyModeState,
	RestoringPrivacyModeState,
	FailedPrivacyModeState
};

struct KPrivacyModeStatus
{
	KPrivacyMode requestedMode = DisabledPrivacyMode;
	KPrivacyMode effectiveMode = DisabledPrivacyMode;
	KPrivacyModeState state = InactivePrivacyModeState;
	QString strRequestId;
	QString strErrorCode;
	quint64 nGeneration = 0;
};

enum KPostSessionAction
{
	UnknownPostSessionAction = -1,
	NoPostSessionAction,
	LockWorkstationPostSessionAction
};

struct KPostSessionActionStatus
{
	KPostSessionAction action = NoPostSessionAction;
	QString strRequestId;
	QString strErrorCode;
	quint64 nGeneration = 0;
};

#endif // _WINREMOTECONTROL_CORE_PRIVACY_PRIVACYTYPES_H_
