#include "input/inputinjector.h"

#include "common/latencytracelogger.h"
#include "core/input/inputinjectorinterface.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>

#include <utility>

namespace
{
	static QString inputTraceExtra(const KInputMessage &message)
	{
		if (message.type == KeyInputMessageType)
		{
			return QStringLiteral("seq=%1 type=%2 pressed=%3")
				.arg(message.nSequence)
				.arg(KInputMessageCodec::typeName(message.type))
				.arg(message.bPressed ? 1 : 0);
		}

		return QStringLiteral("seq=%1 type=%2 x=%3 y=%4")
			.arg(message.nSequence)
			.arg(KInputMessageCodec::typeName(message.type))
			.arg(message.nX)
			.arg(message.nY);
	}
}

KInputInjector::KInputInjector(std::unique_ptr<IKInputInjector> spInputInjector,
	QObject *pParent)
	: QObject(pParent)
	, m_spInputInjector(std::move(spInputInjector))
{
}

KInputInjector::~KInputInjector()
{
	releaseAllInputs();
}

void KInputInjector::handleInputMessage(const KInputMessage &message)
{
	const bool bTrace = message.bTrace
		|| (message.type == KeyInputMessageType && KLatencyTraceLogger::isEnabled());
	QElapsedTimer timer;
	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("inject_begin"),
			inputTraceExtra(message));
		timer.start();
	}

	QString strError;
	const bool bOk = m_spInputInjector != nullptr
		&& m_spInputInjector->inject(message, &strError);
	if (!bOk && !strError.isEmpty())
		emit inputError(strError);
	if (bOk && message.nSequence > 0)
		emit inputInjected(message.nSequence, QDateTime::currentMSecsSinceEpoch());

	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("inject_end"),
			QStringLiteral("%1 costMs=%2 ok=%3")
				.arg(inputTraceExtra(message))
				.arg(timer.isValid() ? timer.elapsed() : -1)
				.arg(bOk ? 1 : 0));
	}
}

void KInputInjector::releaseAllKeys()
{
	if (m_spInputInjector == nullptr)
		return;

	QStringList errorMessages;
	m_spInputInjector->releaseAllKeys(&errorMessages);
	emitInputErrors(errorMessages);
}

void KInputInjector::releaseAllInputs()
{
	if (m_spInputInjector == nullptr)
		return;

	QStringList errorMessages;
	m_spInputInjector->releaseAllInputs(&errorMessages);
	emitInputErrors(errorMessages);
}

void KInputInjector::emitInputErrors(const QStringList &errorMessages)
{
	for (const QString &strError : errorMessages)
	{
		if (!strError.isEmpty())
			emit inputError(strError);
	}
}
