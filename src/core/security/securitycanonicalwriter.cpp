#include "core/security/securitycanonicalwriter.h"

void KSecurityCanonicalWriter::appendUInt8(quint8 nValue)
{
	m_data.append(static_cast<char>(nValue));
}

void KSecurityCanonicalWriter::appendUInt32(quint32 nValue)
{
	for (int nShift = 24; nShift >= 0; nShift -= 8)
		m_data.append(static_cast<char>((nValue >> nShift) & 0xff));
}

void KSecurityCanonicalWriter::appendUInt64(quint64 nValue)
{
	for (int nShift = 56; nShift >= 0; nShift -= 8)
		m_data.append(static_cast<char>((nValue >> nShift) & 0xff));
}

void KSecurityCanonicalWriter::appendBool(bool bValue)
{
	appendUInt8(bValue ? 1 : 0);
}

void KSecurityCanonicalWriter::appendBytes(const QByteArray &value)
{
	appendUInt32(static_cast<quint32>(value.size()));
	m_data.append(value);
}

void KSecurityCanonicalWriter::appendString(const QString &strValue)
{
	appendBytes(strValue.toUtf8());
}

QByteArray KSecurityCanonicalWriter::data() const
{
	return m_data;
}
