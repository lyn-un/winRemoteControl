#include "core/protocol/inputmessage.h"

#include "core/protocol/protocolconstraints.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

namespace
{
	constexpr char kType[] = "type";
	constexpr char kVersion[] = "version";
	constexpr char kMouseMove[] = "mouseMove";
	constexpr char kMouseButton[] = "mouseButton";
	constexpr char kMouseWheel[] = "mouseWheel";
	constexpr char kKey[] = "key";
	constexpr char kText[] = "text";
	constexpr char kButton[] = "button";
	constexpr char kLeft[] = "left";
	constexpr char kRight[] = "right";
	constexpr char kMiddle[] = "middle";
	constexpr char kX1[] = "x1";
	constexpr char kX2[] = "x2";
	constexpr char kPressed[] = "pressed";
	constexpr char kX[] = "x";
	constexpr char kY[] = "y";
	constexpr char kDelta[] = "delta";
	constexpr char kSeq[] = "seq";
	constexpr char kTrace[] = "trace";
	constexpr char kVirtualKey[] = "vk";
	constexpr char kExtended[] = "extended";
	constexpr char kScanCode[] = "scanCode";
	constexpr char kAutoRepeat[] = "autoRepeat";
	constexpr char kValue[] = "value";

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

	bool readOptionalInt(const QJsonObject &object, const char *pName, int *pValue)
	{
		if (object.value(QString::fromLatin1(pName)).isUndefined())
			return true;
		return readRequiredInt(object, pName, pValue);
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

	bool hasSupportedVersion(const QJsonObject &object)
	{
		const QJsonValue value = object.value(QString::fromLatin1(kVersion));
		if (value.isUndefined())
			return true;
		return value.isDouble()
			&& value.toDouble() == static_cast<double>(value.toInt())
			&& value.toInt() == KInputMessageCodec::kProtocolVersion;
	}

	bool isValidMousePosition(int nX, int nY)
	{
		return nX >= 0 && nX <= KProtocolConstraints::kMaximumMouseCoordinate
			&& nY >= 0 && nY <= KProtocolConstraints::kMaximumMouseCoordinate;
	}
}

QString KInputMessageCodec::encode(const KInputMessage &message)
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kVersion), kProtocolVersion);
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
		object.insert(QString::fromLatin1(kScanCode), message.nScanCode);
		object.insert(QString::fromLatin1(kAutoRepeat), message.bAutoRepeat);
	}
	else if (message.type == TextInputMessageType)
	{
		object.insert(QString::fromLatin1(kValue), message.strText);
	}

	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool KInputMessageCodec::decode(const QString &strMessage,
	KInputMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return failDecode(QStringLiteral("Input message output is null"), pErrorMessage);
	const QByteArray data = strMessage.toUtf8();
	if (data.size() > KProtocolConstraints::kMaximumInputMessageBytes)
		return failDecode(QStringLiteral("Input message is too large"), pErrorMessage);

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return failDecode(QStringLiteral("Input message is not a JSON object"), pErrorMessage);

	const QJsonObject object = document.object();
	if (!hasSupportedVersion(object))
		return failDecode(QStringLiteral("Unsupported input protocol version"), pErrorMessage);
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
	else if (strType == QString::fromLatin1(kText))
		message.type = TextInputMessageType;
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
			|| !readRequiredInt(object, kY, &message.nY)
			|| !isValidMousePosition(message.nX, message.nY))
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
			|| !readRequiredInt(object, kY, &message.nY)
			|| !isValidMousePosition(message.nX, message.nY))
		{
			return failDecode(QStringLiteral("Invalid mouse button message"), pErrorMessage);
		}

		const QString strButton = buttonValue.toString();
		if (strButton == QString::fromLatin1(kLeft))
			message.mouseButton = LeftRemoteMouseButton;
		else if (strButton == QString::fromLatin1(kRight))
			message.mouseButton = RightRemoteMouseButton;
		else if (strButton == QString::fromLatin1(kMiddle))
			message.mouseButton = MiddleRemoteMouseButton;
		else if (strButton == QString::fromLatin1(kX1))
			message.mouseButton = X1RemoteMouseButton;
		else if (strButton == QString::fromLatin1(kX2))
			message.mouseButton = X2RemoteMouseButton;
		else
			return failDecode(QStringLiteral("Unknown remote mouse button"), pErrorMessage);
	}
	else if (message.type == MouseWheelInputMessageType)
	{
		if (!readRequiredInt(object, kX, &message.nX)
			|| !readRequiredInt(object, kY, &message.nY)
			|| !readRequiredInt(object, kDelta, &message.nWheelDelta)
			|| !isValidMousePosition(message.nX, message.nY)
			|| message.nWheelDelta < -KProtocolConstraints::kMaximumWheelDelta
			|| message.nWheelDelta > KProtocolConstraints::kMaximumWheelDelta)
		{
			return failDecode(QStringLiteral("Invalid mouse wheel message"), pErrorMessage);
		}
	}
	else if (message.type == KeyInputMessageType)
	{
		if (!readRequiredInt(object, kVirtualKey, &message.nVirtualKey)
			|| !readOptionalInt(object, kScanCode, &message.nScanCode)
			|| !readRequiredBool(object, kPressed, &message.bPressed)
			|| !readOptionalBool(object, kExtended, &message.bExtended)
			|| !readOptionalBool(object, kAutoRepeat, &message.bAutoRepeat)
			|| message.nVirtualKey <= 0
			|| message.nVirtualKey > 0xFF
			|| message.nScanCode < 0 || message.nScanCode > 0x1FF)
		{
			return failDecode(QStringLiteral("Invalid remote key message"), pErrorMessage);
		}
	}
	else if (message.type == TextInputMessageType)
	{
		const QJsonValue textValue = object.value(QString::fromLatin1(kValue));
		if (!textValue.isString() || textValue.toString().isEmpty()
			|| textValue.toString().toUtf8().size() > KProtocolConstraints::kMaximumTextInputBytes)
		{
			return failDecode(QStringLiteral("Invalid remote text message"), pErrorMessage);
		}
		message.strText = textValue.toString();
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
	if (type == TextInputMessageType)
		return QString::fromLatin1(kText);
	return QStringLiteral("invalid");
}

QString KInputMessageCodec::mouseButtonName(KRemoteMouseButton button)
{
	if (button == LeftRemoteMouseButton)
		return QString::fromLatin1(kLeft);
	if (button == RightRemoteMouseButton)
		return QString::fromLatin1(kRight);
	if (button == MiddleRemoteMouseButton)
		return QString::fromLatin1(kMiddle);
	if (button == X1RemoteMouseButton)
		return QString::fromLatin1(kX1);
	if (button == X2RemoteMouseButton)
		return QString::fromLatin1(kX2);
	return QStringLiteral("invalid");
}
