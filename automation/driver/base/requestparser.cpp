#include "automation/driver/base/requestparser.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QStringDecoder>

namespace
{
	bool DecodeUtf8(const QByteArray &bytes, QString *pValue)
	{
		QStringDecoder decoder(QStringDecoder::Utf8);
		const QString value = decoder.decode(bytes);
		if (decoder.hasError())
			return false;
		*pValue = value;
		return true;
	}

	bool Fail(const QString &strMessage, QJsonObject *pErrorResponse)
	{
		if (pErrorResponse != nullptr)
		{
			*pErrorResponse = DriverErrorResponse(InvalidArgumentDriverStatus,
				QStringLiteral("invalid_argument"), strMessage);
		}
		return false;
	}
}

bool KRequestParser::parse(const QByteArray &methodUtf8,
	const QByteArray &pathUtf8,
	const QByteArray &bodyUtf8,
	KParsedDriverRequest *pRequest,
	QJsonObject *pErrorResponse)
{
	if (pRequest == nullptr)
		return Fail(QStringLiteral("Request output is required"), pErrorResponse);
	if (bodyUtf8.size() > kMaximumBodyBytes)
		return Fail(QStringLiteral("Request body exceeds 16 KB"), pErrorResponse);

	KParsedDriverRequest request;
	if (!DecodeUtf8(methodUtf8, &request.strMethod)
		|| !DecodeUtf8(pathUtf8, &request.strPath))
	{
		return Fail(QStringLiteral("Method and path must be valid UTF-8"), pErrorResponse);
	}
	request.strMethod = request.strMethod.trimmed().toUpper();
	if (request.strMethod.isEmpty() || !request.strPath.startsWith(QLatin1Char('/')))
		return Fail(QStringLiteral("Invalid HTTP method or path"), pErrorResponse);

	if (!bodyUtf8.isEmpty())
	{
		QString strBody;
		if (!DecodeUtf8(bodyUtf8, &strBody))
			return Fail(QStringLiteral("Request body must be valid UTF-8"), pErrorResponse);
		QJsonParseError parseError;
		const QJsonDocument document = QJsonDocument::fromJson(bodyUtf8, &parseError);
		if (parseError.error != QJsonParseError::NoError || !document.isObject())
			return Fail(QStringLiteral("Request body must be a JSON object"), pErrorResponse);
		request.parameters = document.object();
	}
	*pRequest = request;
	if (pErrorResponse != nullptr)
		*pErrorResponse = QJsonObject();
	return true;
}
