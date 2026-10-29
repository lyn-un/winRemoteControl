#include "core/protocol/cursorshapemessage.h"
#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <iostream>
#include <limits>

namespace
{
bool Check(bool bPassed, const char *pName)
{
	if (!bPassed)
		std::cerr << "FAILED: " << pName << std::endl;
	return bPassed;
}

KCursorShape MakeShape(KCursorShapeKind kind, quint64 nSequence = 1)
{
	KCursorShape shape;
	shape.kind = kind;
	shape.nSequence = nSequence;
	shape.nGeneration = 99;
	if (kind != DefaultCursorShape)
	{
		shape.nWidth = 256;
		shape.nHeight = 256;
		shape.nStride = kind == MonochromeCursorShape ? 36 : 1024;
		shape.nHotspotX = 13;
		shape.nHotspotY = 27;
		shape.data.resize(shape.expectedByteCount());
		for (qsizetype nIndex = 0; nIndex < shape.data.size(); ++nIndex)
			shape.data[nIndex] = static_cast<char>((nIndex * 37 + 17) % 256);
	}
	return shape;
}

bool TestRoundTrip()
{
	for (const auto kind : { DefaultCursorShape, ColorCursorShape, MonochromeCursorShape, MaskedColorCursorShape })
	{
		KCursorShape shape = MakeShape(kind, std::numeric_limits<quint64>::max());
		shape.bVisible = kind != DefaultCursorShape;
		const auto messages = KCursorShapeMessageCodec::split(shape);
		if (!Check(!messages.isEmpty() && messages.size() <= 32, "bounded fragment count"))
			return false;
		KCursorShapeAssembler assembler;
		KCursorShape result;
		QString strError;
		for (qsizetype nIndex = 0; nIndex < messages.size(); ++nIndex)
		{
			const QString strEncoded = KCursorShapeMessageCodec::encode(messages[nIndex]);
			KCursorShapeMessage decoded;
			if (!Check(!strEncoded.isEmpty() && strEncoded.toUtf8().size()
				< KProtocolConstraints::kMaximumSessionMessageBytes, "channel limit")
				|| !Check(KCursorShapeMessageCodec::decode(strEncoded, &decoded, &strError), "decode round trip")
				|| !Check(decoded.shape.nGeneration == 0 && decoded.shape.nSequence == shape.nSequence,
					"sequence precision and no remote generation"))
			{
				return false;
			}
			const auto status = assembler.accept(decoded, nIndex, &result, &strError);
			const auto expected = nIndex + 1 == messages.size()
				? KCursorShapeAssembler::CompleteResult : KCursorShapeAssembler::IncompleteResult;
			if (!Check(status == expected, "assembly result")
				|| !Check(assembler.pendingBytes() <= KCursorShape::kMaximumBytes, "bounded assembly bytes"))
			{
				return false;
			}
		}
		if (!Check(result.isValid() && result.hasSameMetadata(shape) && result.data == shape.data,
			"completed shape equality") || !Check(assembler.pendingBytes() == 0, "completion clears pending"))
		{
			return false;
		}
	}
	return true;
}

bool TestShapeValidation()
{
	const KCursorShape base = MakeShape(ColorCursorShape);
	for (int nCase = 0; nCase < 12; ++nCase)
	{
		KCursorShape shape = base;
		switch (nCase)
		{
		case 0: shape.nSequence = 0; break;
		case 1: shape.nWidth = 0; break;
		case 2: shape.nWidth = 257; break;
		case 3: shape.nHeight = std::numeric_limits<int>::max(); break;
		case 4: shape.nStride = std::numeric_limits<int>::max(); break;
		case 5: shape.nStride = 1023; break;
		case 6: shape.nStride = 2048; break;
		case 7: shape.nHotspotX = -1; break;
		case 8: shape.nHotspotY = 256; break;
		case 9: shape.data.chop(1); break;
		case 10: shape.kind = static_cast<KCursorShapeKind>(99); break;
		case 11: shape.kind = DefaultCursorShape; break;
		}
		if (!Check(!shape.isValid() && KCursorShapeMessageCodec::split(shape).isEmpty(), "invalid shape rejected"))
			return false;
	}
	KCursorShape mono = MakeShape(MonochromeCursorShape);
	mono.nWidth = 17;
	mono.nHeight = 11;
	mono.nHotspotX = 16;
	mono.nHotspotY = 10;
	mono.nStride = 4;
	mono.data.resize(88);
	return Check(mono.isValid(), "odd monochrome dimensions and padded stride");
}

bool TestWireValidation()
{
	const auto messages = KCursorShapeMessageCodec::split(MakeShape(ColorCursorShape));
	const QJsonObject base = QJsonDocument::fromJson(KCursorShapeMessageCodec::encode(messages[0]).toUtf8()).object();
	const QVector<QPair<QString, QJsonValue>> invalidFields = {
		{ QStringLiteral("width"), -1 }, { QStringLiteral("height"), 257 },
		{ QStringLiteral("stride"), 1e30 }, { QStringLiteral("hotspotX"), 0.5 },
		{ QStringLiteral("hotspotY"), 256 }, { QStringLiteral("visible"), 1 },
		{ QStringLiteral("kind"), 4 }, { QStringLiteral("offset"), 1 },
		{ QStringLiteral("offset"), 262144 }, { QStringLiteral("totalBytes"), 262145 },
		{ QStringLiteral("version"), 2 }, { QStringLiteral("seq"), QStringLiteral("18446744073709551616") },
		{ QStringLiteral("seq"), QStringLiteral("0") }, { QStringLiteral("data"), QStringLiteral("!!!!") },
		{ QStringLiteral("data"), QString(12000, QLatin1Char('A')) },
		{ QStringLiteral("data"), QStringLiteral("AA==\n") }, { QStringLiteral("data"), QStringLiteral("AA") },
		{ QStringLiteral("data"), QJsonValue(QJsonValue::Null) },
		{ QStringLiteral("type"), QStringLiteral("other") }
	};
	for (const auto &field : invalidFields)
	{
		QJsonObject object = base;
		object.insert(field.first, field.second);
		KCursorShapeMessage decoded;
		QString strError;
		if (!Check(!KCursorShapeMessageCodec::decode(QString::fromUtf8(QJsonDocument(object).toJson()),
			&decoded, &strError) && !strError.isEmpty(), "malformed wire field rejected"))
		{
			return false;
		}
	}
	KProtocolEnvelope envelope;
	QString strError;
	KCursorShapeMessage decoded;
	if (!KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
		KCursorShapeMessageCodec::encode(messages[0]), &envelope, &strError))
	{
		return false;
	}
	envelope.channel = InputProtocolChannel;
	return Check(!KCursorShapeMessageCodec::decode(envelope, &decoded, &strError), "wrong channel rejected");
}

bool TestLifecycle()
{
	const auto messages = KCursorShapeMessageCodec::split(MakeShape(ColorCursorShape));
	KCursorShapeAssembler assembler;
	KCursorShape result;
	QString strError;
	const auto Accept = [&](const KCursorShapeMessage &message, qint64 nNowMs)
	{
		return assembler.accept(message, nNowMs, &result, &strError);
	};
	if (!Check(Accept(messages[1], 0) == KCursorShapeAssembler::RejectedResult, "missing first chunk")
		|| !Check(Accept(messages[0], 10) == KCursorShapeAssembler::IncompleteResult, "begin assembly")
		|| !Check(Accept(messages[2], 11) == KCursorShapeAssembler::RejectedResult, "missing middle chunk")
		|| !Check(assembler.pendingBytes() == 0, "bad order releases data")
		|| !Check(Accept(messages[0], 12) == KCursorShapeAssembler::IgnoredResult, "failed sequence cannot restart"))
	{
		return false;
	}
	assembler.reset();
	Accept(messages[0], 0);
	assembler.expire(4999);
	if (!Check(assembler.pendingBytes() == 8192, "timeout not early"))
		return false;
	assembler.expire(5000);
	if (!Check(assembler.pendingBytes() == 0, "timeout releases data")
		|| !Check(Accept(messages[1], 5001) == KCursorShapeAssembler::IgnoredResult, "late expired chunk ignored"))
	{
		return false;
	}
	assembler.reset();
	Accept(messages[0], 0);
	KCursorShapeMessage changed = messages[1];
	changed.shape.bVisible = false;
	if (!Check(Accept(changed, 1) == KCursorShapeAssembler::RejectedResult, "metadata mismatch"))
		return false;
	assembler.reset();
	Accept(messages[0], 0);
	if (!Check(Accept(messages[0], 1) == KCursorShapeAssembler::RejectedResult, "duplicate chunk rejected"))
		return false;
	assembler.reset();
	Accept(messages[0], 0);
	const auto newer = KCursorShapeMessageCodec::split(MakeShape(DefaultCursorShape, 2));
	if (!Check(Accept(newer[0], 1) == KCursorShapeAssembler::CompleteResult, "new shape replaces assembly")
		|| !Check(assembler.pendingBytes() == 0, "replacement bounded")
		|| !Check(Accept(messages[1], 2) == KCursorShapeAssembler::IgnoredResult, "old chunk ignored")
		|| !Check(Accept(newer[0], 3) == KCursorShapeAssembler::IgnoredResult, "completed duplicate ignored"))
	{
		return false;
	}
	assembler.reset();
	if (!Check(Accept(messages[0], 100) == KCursorShapeAssembler::IncompleteResult, "reset accepts old sequence"))
		return false;
	return Check(Accept(messages[1], 99) == KCursorShapeAssembler::RejectedResult
		&& assembler.pendingBytes() == 0, "invalid time releases assembly");
}
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	if (!TestRoundTrip() || !TestShapeValidation() || !TestWireValidation() || !TestLifecycle())
		return 1;
	std::cout << "cursor shape: formats, fragmentation, strict validation, bounds and lifecycle passed" << std::endl;
	return 0;
}
