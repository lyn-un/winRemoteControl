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
		if (message.type == MouseButtonInputMessageType)
		{
			return QStringLiteral(
				"seq=%1 type=%2 channel=%3 button=%4 pressed=%5 x=%6 y=%7")
				.arg(message.nSequence)
				.arg(KInputMessageCodec::typeName(message.type))
				.arg(message.bRealtime ? QStringLiteral("realtime")
					: QStringLiteral("reliable"))
				.arg(KInputMessageCodec::mouseButtonName(message.mouseButton))
				.arg(message.bPressed ? 1 : 0)
				.arg(message.nX)
				.arg(message.nY);
		}

		return QStringLiteral("seq=%1 type=%2 channel=%3 x=%4 y=%5")
			.arg(message.nSequence)
			.arg(KInputMessageCodec::typeName(message.type))
			.arg(message.bRealtime ? QStringLiteral("realtime")
				: QStringLiteral("reliable"))
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
	quint64 &nLastSequence = message.type == MouseMoveInputMessageType
		? m_nLastPointerSequence : m_nLastReliableSequence;
	if (message.nSequence > 0 && message.nSequence <= nLastSequence)
		return;
	if (message.nSequence > 0)
		nLastSequence = message.nSequence;
	if (m_bOrderedMouseDragEnabled
		&& message.type == MouseMoveInputMessageType
		&& message.bRealtime
		&& !m_pressedMouseButtons.isEmpty())
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("input_drop"),
			QStringLiteral("%1 reason=late_realtime_move pressedButtons=%2")
				.arg(inputTraceExtra(message))
				.arg(m_pressedMouseButtons.size()));
		return;
	}
	const bool bTrace = message.bTrace
		|| ((message.type == KeyInputMessageType || message.type == TextInputMessageType)
			&& KLatencyTraceLogger::isEnabled());
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
	if (bOk && message.type == MouseButtonInputMessageType)
	{
		const int nButton = static_cast<int>(message.mouseButton);
		if (message.bPressed)
			m_pressedMouseButtons.insert(nButton);
		else
			m_pressedMouseButtons.remove(nButton);
	}

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

void KInputInjector::setOrderedMouseDragEnabled(bool bEnabled)
{
	m_bOrderedMouseDragEnabled = bEnabled;
	m_pressedMouseButtons.clear();
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
	m_nLastPointerSequence = 0;
	m_nLastReliableSequence = 0;
	m_pressedMouseButtons.clear();
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
