#include "core/protocol/inputmessage.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

namespace
{
	constexpr char kType[] = "type";
	constexpr char kMouseMove[] = "mouseMove";
	constexpr char kMouseButton[] = "mouseButton";
	constexpr char kMouseWheel[] = "mouseWheel";
	constexpr char kKey[] = "key";
	constexpr char kButton[] = "button";
	constexpr char kLeft[] = "left";
	constexpr char kRight[] = "right";
	constexpr char kPressed[] = "pressed";
	constexpr char kX[] = "x";
	constexpr char kY[] = "y";
	constexpr char kDelta[] = "delta";
	constexpr char kSeq[] = "seq";
	constexpr char kTrace[] = "trace";
	constexpr char kVirtualKey[] = "vk";
	constexpr char kExtended[] = "extended";

	bool readRequiredInt(const QJsonObject &object, const char *pName, int *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isDouble())
			return false;

		const int nValue = value.toInt();
		if (static_cast<double>(nValue) != value.toDouble())
			return false;

		*pValue = nValue;
		return true;
	}

	bool readRequiredBool(const QJsonObject &object, const char *pName, bool *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isBool())
			return false;

		*pValue = value.toBool();
		return true;
	}

	bool readOptionalBool(const QJsonObject &object, const char *pName, bool *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (value.isUndefined())
			return true;
		if (!value.isBool())
			return false;

		*pValue = value.toBool();
		return true;
	}

	bool readSequence(const QJsonObject &object, quint64 *pSequence)
	{
		const QJsonValue value = object.value(QString::fromLatin1(kSeq));
		if (value.isUndefined())
			return true;
		if (!value.isString())
			return false;

		bool bOk = false;
		const quint64 nSequence = value.toString().toULongLong(&bOk);
		if (!bOk)
			return false;

		*pSequence = nSequence;
		return true;
	}

	bool failDecode(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}
}

QString KInputMessageCodec::encode(const KInputMessage &message)
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kType), typeName(message.type));
	object.insert(QString::fromLatin1(kSeq), QString::number(message.nSequence));
	if (message.bTrace)
		object.insert(QString::fromLatin1(kTrace), true);

	if (message.type == MouseMoveInputMessageType)
	{
		object.insert(QString::fromLatin1(kX), message.nX);
		object.insert(QString::fromLatin1(kY), message.nY);
	}
	else if (message.type == MouseButtonInputMessageType)
	{
		object.insert(QString::fromLatin1(kButton), mouseButtonName(message.mouseButton));
		object.insert(QString::fromLatin1(kPressed), message.bPressed);
		object.insert(QString::fromLatin1(kX), message.nX);
		object.insert(QString::fromLatin1(kY), message.nY);
	}
	else if (message.type == MouseWheelInputMessageType)
	{
		object.insert(QString::fromLatin1(kDelta), message.nWheelDelta);
		object.insert(QString::fromLatin1(kX), message.nX);
		object.insert(QString::fromLatin1(kY), message.nY);
	}
	else if (message.type == KeyInputMessageType)
	{
		object.insert(QString::fromLatin1(kVirtualKey), message.nVirtualKey);
		object.insert(QString::fromLatin1(kPressed), message.bPressed);
		object.insert(QString::fromLatin1(kExtended), message.bExtended);
	}

	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool KInputMessageCodec::decode(const QString &strMessage,
	KInputMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return failDecode(QStringLiteral("Input message output is null"), pErrorMessage);

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return failDecode(QStringLiteral("Input message is not a JSON object"), pErrorMessage);

	const QJsonObject object = document.object();
	const QString strType = object.value(QString::fromLatin1(kType)).toString();
	KInputMessage message;
	if (strType == QString::fromLatin1(kMouseMove))
		message.type = MouseMoveInputMessageType;
	else if (strType == QString::fromLatin1(kMouseButton))
		message.type = MouseButtonInputMessageType;
	else if (strType == QString::fromLatin1(kMouseWheel))
		message.type = MouseWheelInputMessageType;
	else if (strType == QString::fromLatin1(kKey))
		message.type = KeyInputMessageType;
	else
		return failDecode(QStringLiteral("Unknown input message type"), pErrorMessage);

	if (!readSequence(object, &message.nSequence)
		|| !readOptionalBool(object, kTrace, &message.bTrace))
	{
		return failDecode(QStringLiteral("Invalid input message metadata"), pErrorMessage);
	}

	if (message.type == MouseMoveInputMessageType)
	{
		if (!readRequiredInt(object, kX, &message.nX)
			|| !readRequiredInt(object, kY, &message.nY))
		{
			return failDecode(QStringLiteral("Invalid mouse move message"), pErrorMessage);
		}
	}
	else if (message.type == MouseButtonInputMessageType)
	{
		const QJsonValue buttonValue = object.value(QString::fromLatin1(kButton));
		if (!buttonValue.isString()
			|| !readRequiredBool(object, kPressed, &message.bPressed)
			|| !readRequiredInt(object, kX, &message.nX)
			|| !readRequiredInt(object, kY, &message.nY))
		{
			return failDecode(QStringLiteral("Invalid mouse button message"), pErrorMessage);
		}

		const QString strButton = buttonValue.toString();
		if (strButton == QString::fromLatin1(kLeft))
			message.mouseButton = LeftRemoteMouseButton;
		else if (strButton == QString::fromLatin1(kRight))
			message.mouseButton = RightRemoteMouseButton;
		else
			return failDecode(QStringLiteral("Unknown remote mouse button"), pErrorMessage);
	}
	else if (message.type == MouseWheelInputMessageType)
	{
		if (!readRequiredInt(object, kX, &message.nX)
			|| !readRequiredInt(object, kY, &message.nY)
			|| !readRequiredInt(object, kDelta, &message.nWheelDelta))
		{
			return failDecode(QStringLiteral("Invalid mouse wheel message"), pErrorMessage);
		}
	}
	else if (message.type == KeyInputMessageType)
	{
		if (!readRequiredInt(object, kVirtualKey, &message.nVirtualKey)
			|| !readRequiredBool(object, kPressed, &message.bPressed)
			|| !readOptionalBool(object, kExtended, &message.bExtended)
			|| message.nVirtualKey <= 0
			|| message.nVirtualKey > 0xFF)
		{
			return failDecode(QStringLiteral("Invalid remote key message"), pErrorMessage);
		}
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KInputMessageCodec::typeName(KInputMessageType type)
{
	if (type == MouseMoveInputMessageType)
		return QString::fromLatin1(kMouseMove);
	if (type == MouseButtonInputMessageType)
		return QString::fromLatin1(kMouseButton);
	if (type == MouseWheelInputMessageType)
		return QString::fromLatin1(kMouseWheel);
	if (type == KeyInputMessageType)
		return QString::fromLatin1(kKey);
	return QStringLiteral("invalid");
}

QString KInputMessageCodec::mouseButtonName(KRemoteMouseButton button)
{
	if (button == LeftRemoteMouseButton)
		return QString::fromLatin1(kLeft);
	if (button == RightRemoteMouseButton)
		return QString::fromLatin1(kRight);
	return QStringLiteral("invalid");
}
