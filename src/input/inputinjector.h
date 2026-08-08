#ifndef _WINREMOTECONTROL_INPUTINJECTOR_H_
#define _WINREMOTECONTROL_INPUTINJECTOR_H_

#include "core/protocol/inputmessage.h"

#include <QtCore/QObject>
#include <QtCore/QStringList>

#include <memory>

class IKInputInjector;

class KInputInjector : public QObject
{
	Q_OBJECT

public:
	explicit KInputInjector(std::unique_ptr<IKInputInjector> spInputInjector,
		QObject *pParent = nullptr);
	~KInputInjector() override;

	KInputInjector(const KInputInjector &) = delete;
	KInputInjector &operator=(const KInputInjector &) = delete;

public slots:
	void handleInputMessage(const KInputMessage &message);
	void releaseAllKeys();
	void releaseAllInputs();

signals:
	void inputError(const QString &strMessage);
	void inputInjected(quint64 nSeq, qint64 nInjectedMs);

private:
	void emitInputErrors(const QStringList &errorMessages);

	std::unique_ptr<IKInputInjector> m_spInputInjector;
	quint64 m_nLastPointerSequence = 0;
	quint64 m_nLastReliableSequence = 0;
};

#endif // _WINREMOTECONTROL_INPUTINJECTOR_H_
