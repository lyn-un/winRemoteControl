#ifndef _WINREMOTECONTROL_INPUTINJECTORINTERFACE_H_
#define _WINREMOTECONTROL_INPUTINJECTORINTERFACE_H_

#include "core/protocol/inputmessage.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

class IKInputInjector
{
public:
	virtual ~IKInputInjector() = default;

	virtual bool inject(const KInputMessage &message, QString *pErrorMessage) = 0;
	virtual void releaseAllKeys(QStringList *pErrorMessages) = 0;
	virtual void releaseAllInputs(QStringList *pErrorMessages) = 0;
};

#endif // _WINREMOTECONTROL_INPUTINJECTORINTERFACE_H_
