#ifndef _WINREMOTECONTROL_CORE_SECURITY_SECURITYCANONICALWRITER_H_
#define _WINREMOTECONTROL_CORE_SECURITY_SECURITYCANONICALWRITER_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

class KSecurityCanonicalWriter
{
public:
	void appendUInt8(quint8 nValue);
	void appendUInt32(quint32 nValue);
	void appendUInt64(quint64 nValue);
	void appendBool(bool bValue);
	void appendBytes(const QByteArray &value);
	void appendString(const QString &strValue);

	QByteArray data() const;

private:
	QByteArray m_data;
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_SECURITYCANONICALWRITER_H_
