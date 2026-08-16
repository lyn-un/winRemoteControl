#include "adapters/signaling/schanneltlsengine.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <algorithm>
#include <cstring>

namespace
{
	constexpr DWORD kTls12ProtocolVersion = 0x0303;
	constexpr DWORD kTls13ProtocolVersion = 0x0304;
	constexpr ULONG kClientContextRequirements = ISC_REQ_SEQUENCE_DETECT
		| ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR
		| ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
	constexpr ULONG kServerContextRequirements = ASC_REQ_SEQUENCE_DETECT
		| ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR
		| ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH;

	QString SecurityStatusMessage(const QString &strOperation, SECURITY_STATUS status)
	{
		return QStringLiteral("%1 failed (0x%2)").arg(strOperation)
			.arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
	}

	QDateTime DateTimeFromFileTime(const FILETIME &fileTime)
	{
		SYSTEMTIME systemTime = {};
		if (!FileTimeToSystemTime(&fileTime, &systemTime))
			return QDateTime();
		return QDateTime(QDate(systemTime.wYear, systemTime.wMonth, systemTime.wDay),
			QTime(systemTime.wHour, systemTime.wMinute, systemTime.wSecond,
				systemTime.wMilliseconds), Qt::UTC);
	}

	bool EncodeSpki(PCCERT_CONTEXT pCertificate, QByteArray *pDer,
		QString *pErrorMessage)
	{
		DWORD nBytes = 0;
		if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
			&pCertificate->pCertInfo->SubjectPublicKeyInfo, 0, nullptr,
			nullptr, &nBytes))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Unable to encode peer SPKI (%1)")
					.arg(GetLastError());
			return false;
		}
		QByteArray der(static_cast<int>(nBytes), Qt::Uninitialized);
		if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
			&pCertificate->pCertInfo->SubjectPublicKeyInfo, 0, nullptr,
			der.data(), &nBytes))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Unable to encode peer SPKI (%1)")
					.arg(GetLastError());
			return false;
		}
		der.resize(static_cast<int>(nBytes));
		*pDer = der;
		return true;
	}

	bool HasEnhancedUsage(PCCERT_CONTEXT pCertificate, LPCSTR pszUsage)
	{
		DWORD nBytes = 0;
		if (!CertGetEnhancedKeyUsage(pCertificate, 0, nullptr, &nBytes)
			|| nBytes < sizeof(CERT_ENHKEY_USAGE))
		{
			return false;
		}
		QByteArray storage(static_cast<int>(nBytes), Qt::Uninitialized);
		auto *pUsage = reinterpret_cast<PCERT_ENHKEY_USAGE>(storage.data());
		if (!CertGetEnhancedKeyUsage(pCertificate, 0, pUsage, &nBytes))
			return false;
		for (DWORD nIndex = 0; nIndex < pUsage->cUsageIdentifier; ++nIndex)
		{
			if (strcmp(pUsage->rgpszUsageIdentifier[nIndex], pszUsage) == 0)
				return true;
		}
		return false;
	}

	bool IsP256PublicKey(const CRYPT_ALGORITHM_IDENTIFIER &algorithm)
	{
		if (algorithm.pszObjId == nullptr
			|| strcmp(algorithm.pszObjId, szOID_ECC_PUBLIC_KEY) != 0
			|| algorithm.Parameters.cbData == 0)
		{
			return false;
		}
		DWORD nBytes = 0;
		if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_OBJECT_IDENTIFIER,
			algorithm.Parameters.pbData, algorithm.Parameters.cbData,
			0, nullptr, nullptr, &nBytes))
		{
			return false;
		}
		QByteArray decoded(static_cast<int>(nBytes), Qt::Uninitialized);
		if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_OBJECT_IDENTIFIER,
			algorithm.Parameters.pbData, algorithm.Parameters.cbData,
			0, nullptr, decoded.data(), &nBytes))
		{
			return false;
		}
		const auto pszCurve = *reinterpret_cast<LPCSTR *>(decoded.data());
		return pszCurve != nullptr && strcmp(pszCurve, szOID_ECC_CURVE_P256) == 0;
	}

	QString DeviceIdFromCertificate(PCCERT_CONTEXT pCertificate)
	{
		DWORD nCharacters = CertGetNameStringW(pCertificate, CERT_NAME_URL_TYPE,
			0, nullptr, nullptr, 0);
		if (nCharacters <= 1)
			return QString();
		QString strUri(static_cast<int>(nCharacters), Qt::Uninitialized);
		if (CertGetNameStringW(pCertificate, CERT_NAME_URL_TYPE, 0, nullptr,
			reinterpret_cast<LPWSTR>(strUri.data()), nCharacters) != nCharacters)
		{
			return QString();
		}
		strUri.chop(1);
		const QString strPrefix = QStringLiteral("urn:wrc:device:");
		if (!strUri.startsWith(strPrefix))
			return QString();
		const QString strDeviceId = strUri.mid(strPrefix.size());
		return QUuid(strDeviceId).isNull() ? QString() : strDeviceId;
	}

	bool ValidatePeerCertificate(PCCERT_CONTEXT pCertificate,
		QString *pDeviceId,
		QString *pErrorMessage)
	{
		if (pCertificate == nullptr
			|| !CertCompareCertificateName(X509_ASN_ENCODING,
				&pCertificate->pCertInfo->Subject, &pCertificate->pCertInfo->Issuer)
			|| CertVerifyTimeValidity(nullptr, pCertificate->pCertInfo) != 0
			|| !CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING,
				CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
				const_cast<PCERT_CONTEXT>(pCertificate),
				CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT,
				const_cast<PCERT_CONTEXT>(pCertificate),
				0, nullptr))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Peer certificate is not a valid current self-signed certificate");
			return false;
		}
		const CRYPT_ALGORITHM_IDENTIFIER &algorithm =
			pCertificate->pCertInfo->SubjectPublicKeyInfo.Algorithm;
		if (!IsP256PublicKey(algorithm)
			|| !HasEnhancedUsage(pCertificate, szOID_PKIX_KP_CLIENT_AUTH)
			|| !HasEnhancedUsage(pCertificate, szOID_PKIX_KP_SERVER_AUTH))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Peer certificate algorithm or enhanced usage is invalid");
			return false;
		}
		BYTE nKeyUsage = 0;
		if (!CertGetIntendedKeyUsage(X509_ASN_ENCODING, pCertificate->pCertInfo,
			&nKeyUsage, sizeof(nKeyUsage))
			|| (nKeyUsage & CERT_DIGITAL_SIGNATURE_KEY_USAGE) == 0)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Peer certificate cannot be used for digital signatures");
			return false;
		}
		const QString strDeviceId = DeviceIdFromCertificate(pCertificate);
		if (strDeviceId.isEmpty())
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Peer certificate has no valid device identity");
			return false;
		}
		*pDeviceId = strDeviceId;
		return true;
	}

	QString ProtocolName(DWORD nProtocol)
	{
		if (nProtocol == kTls13ProtocolVersion)
			return QStringLiteral("TLS1.3");
		if (nProtocol == kTls12ProtocolVersion)
			return QStringLiteral("TLS1.2");
		return QString();
	}

	bool IsAllowedCipher(const SecPkgContext_CipherInfo &cipherInfo)
	{
		const QString strSuite = QString::fromWCharArray(cipherInfo.szCipherSuite).toUpper();
		const bool bAead = strSuite.contains(QStringLiteral("_GCM_"))
			|| strSuite.contains(QStringLiteral("CHACHA20_POLY1305"));
		if (!bAead)
			return false;
		if (cipherInfo.dwProtocol == kTls13ProtocolVersion)
			return true;
		return cipherInfo.dwProtocol == kTls12ProtocolVersion
			&& QString::fromWCharArray(cipherInfo.szExchange).toUpper()
				.contains(QStringLiteral("ECDH"));
	}
}

KSchannelTlsEngine::KSchannelTlsEngine() = default;

KSchannelTlsEngine::~KSchannelTlsEngine()
{
	clear();
}

bool KSchannelTlsEngine::initialize(bool bServer,
	void *pCertificateContext,
	QString *pErrorMessage)
{
	clear();
	if (pCertificateContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("A local device certificate is required");
		return false;
	}
	m_bServer = bServer;
	m_pLocalCertificate = static_cast<PCCERT_CONTEXT>(pCertificateContext);
	HCRYPTPROV_OR_NCRYPT_KEY_HANDLE hPrivateKey = 0;
	DWORD nKeySpec = 0;
	BOOL bCallerMustFree = FALSE;
	if (!CryptAcquireCertificatePrivateKey(m_pLocalCertificate,
		CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
		nullptr, &hPrivateKey, &nKeySpec, &bCallerMustFree))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to acquire the device certificate private key (%1)")
				.arg(GetLastError());
		clear();
		return false;
	}
	if (bCallerMustFree)
		NCryptFreeObject(hPrivateKey);
	TLS_PARAMETERS tlsParameters = {};
	tlsParameters.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3
		| SP_PROT_TLS1 | SP_PROT_TLS1_1;
	PCCERT_CONTEXT pCertificate = m_pLocalCertificate;
	SCH_CREDENTIALS credentials = {};
	credentials.dwVersion = SCH_CREDENTIALS_VERSION;
	credentials.dwCredFormat = SCH_CRED_FORMAT_CERT_CONTEXT;
	credentials.cCreds = 1;
	credentials.paCred = &pCertificate;
	credentials.dwFlags = SCH_USE_STRONG_CRYPTO;
	if (bServer)
		credentials.dwFlags |= SCH_CRED_NO_SYSTEM_MAPPER | SCH_CRED_DISABLE_RECONNECTS;
	else
		credentials.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION;
	credentials.cTlsParameters = 1;
	credentials.pTlsParameters = &tlsParameters;
	TimeStamp expiry = {};
	const SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr,
		const_cast<LPWSTR>(UNISP_NAME_W),
		bServer ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND,
		nullptr, &credentials, nullptr, nullptr, &m_credential, &expiry);
	if (status != SEC_E_OK)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = SecurityStatusMessage(QStringLiteral("AcquireCredentialsHandle"), status);
		clear();
		return false;
	}
	m_bCredentialValid = true;
	return true;
}

bool KSchannelTlsEngine::start(QList<QByteArray> *pOutputRecords,
	QString *pErrorMessage)
{
	if (m_bServer)
		return true;
	QByteArray input;
	bool bCompleted = false;
	return handshakeStep(&input, true, pOutputRecords, &bCompleted, pErrorMessage);
}

bool KSchannelTlsEngine::continueHandshake(QByteArray *pEncryptedBuffer,
	QList<QByteArray> *pOutputRecords,
	bool *pCompleted,
	QString *pErrorMessage)
{
	return handshakeStep(pEncryptedBuffer, !m_bContextValid,
		pOutputRecords, pCompleted, pErrorMessage);
}

bool KSchannelTlsEngine::handshakeStep(QByteArray *pEncryptedBuffer,
	bool bInitial,
	QList<QByteArray> *pOutputRecords,
	bool *pCompleted,
	QString *pErrorMessage)
{
	if (!m_bCredentialValid || pEncryptedBuffer == nullptr
		|| pOutputRecords == nullptr || pCompleted == nullptr)
	{
		return false;
	}
	*pCompleted = false;
	SecBuffer inputBuffers[2] = {};
	SecBufferDesc inputDescription = { SECBUFFER_VERSION, 2, inputBuffers };
	inputBuffers[0].BufferType = SECBUFFER_TOKEN;
	inputBuffers[0].pvBuffer = pEncryptedBuffer->data();
	inputBuffers[0].cbBuffer = static_cast<unsigned long>(pEncryptedBuffer->size());
	inputBuffers[1].BufferType = SECBUFFER_EMPTY;
	SecBuffer outputBuffer = { 0, SECBUFFER_TOKEN, nullptr };
	SecBufferDesc outputDescription = { SECBUFFER_VERSION, 1, &outputBuffer };
	ULONG nAttributes = 0;
	TimeStamp expiry = {};
	SECURITY_STATUS status = SEC_E_INTERNAL_ERROR;
	if (m_bServer)
	{
		status = AcceptSecurityContext(&m_credential,
			bInitial ? nullptr : &m_context,
			&inputDescription, kServerContextRequirements,
			SECURITY_NATIVE_DREP, &m_context, &outputDescription,
			&nAttributes, &expiry);
	}
	else
	{
		status = InitializeSecurityContextW(&m_credential,
			bInitial ? nullptr : &m_context, nullptr,
			kClientContextRequirements, 0, SECURITY_NATIVE_DREP,
			bInitial ? nullptr : &inputDescription, 0, &m_context,
			&outputDescription, &nAttributes, &expiry);
	}
	if (outputBuffer.pvBuffer != nullptr && outputBuffer.cbBuffer > 0)
	{
		pOutputRecords->append(QByteArray(static_cast<const char *>(outputBuffer.pvBuffer),
			static_cast<int>(outputBuffer.cbBuffer)));
		FreeContextBuffer(outputBuffer.pvBuffer);
	}
	if (status == SEC_E_INCOMPLETE_MESSAGE)
		return true;
	m_bContextValid = true;
	if (!pEncryptedBuffer->isEmpty())
	{
		if (inputBuffers[1].BufferType == SECBUFFER_EXTRA)
		{
			const qsizetype nExtra = inputBuffers[1].cbBuffer;
			pEncryptedBuffer->remove(0, pEncryptedBuffer->size() - nExtra);
		}
		else
		{
			pEncryptedBuffer->clear();
		}
	}
	if (status == SEC_E_OK)
	{
		if (!finishHandshake(pErrorMessage))
			return false;
		*pCompleted = true;
		return true;
	}
	if (status == SEC_I_CONTINUE_NEEDED)
		return true;
	if (pErrorMessage != nullptr)
		*pErrorMessage = SecurityStatusMessage(QStringLiteral("Schannel handshake"), status);
	return false;
}

bool KSchannelTlsEngine::finishHandshake(QString *pErrorMessage)
{
	if (QueryContextAttributesW(&m_context, SECPKG_ATTR_STREAM_SIZES,
		&m_streamSizes) != SEC_E_OK)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to query Schannel stream sizes");
		return false;
	}
	SecPkgContext_CipherInfo cipherInfo = {};
	cipherInfo.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
	const SECURITY_STATUS cipherStatus = QueryContextAttributesW(&m_context,
		SECPKG_ATTR_CIPHER_INFO, &cipherInfo);
	if (cipherStatus != SEC_E_OK || !IsAllowedCipher(cipherInfo))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("TLS negotiated a disallowed protocol or cipher suite");
		return false;
	}
	m_bReady = true;
	return true;
}

bool KSchannelTlsEngine::encrypt(const QByteArray &plainText,
	QList<QByteArray> *pOutputRecords,
	QString *pErrorMessage)
{
	if (!m_bReady || pOutputRecords == nullptr)
		return false;
	qsizetype nOffset = 0;
	while (nOffset < plainText.size())
	{
		const qsizetype nChunk = std::min<qsizetype>(
			plainText.size() - nOffset, m_streamSizes.cbMaximumMessage);
		QByteArray record(static_cast<int>(m_streamSizes.cbHeader + nChunk
			+ m_streamSizes.cbTrailer), Qt::Uninitialized);
		memcpy(record.data() + m_streamSizes.cbHeader,
			plainText.constData() + nOffset, static_cast<size_t>(nChunk));
		SecBuffer buffers[4] = {
			{ m_streamSizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data() },
			{ static_cast<unsigned long>(nChunk), SECBUFFER_DATA,
				record.data() + m_streamSizes.cbHeader },
			{ m_streamSizes.cbTrailer, SECBUFFER_STREAM_TRAILER,
				record.data() + m_streamSizes.cbHeader + nChunk },
			{ 0, SECBUFFER_EMPTY, nullptr }
		};
		SecBufferDesc description = { SECBUFFER_VERSION, 4, buffers };
		const SECURITY_STATUS status = EncryptMessage(&m_context, 0, &description, 0);
		if (status != SEC_E_OK)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = SecurityStatusMessage(QStringLiteral("EncryptMessage"), status);
			return false;
		}
		record.resize(static_cast<int>(buffers[0].cbBuffer + buffers[1].cbBuffer
			+ buffers[2].cbBuffer));
		pOutputRecords->append(record);
		nOffset += nChunk;
	}
	return true;
}

bool KSchannelTlsEngine::decrypt(QByteArray *pEncryptedBuffer,
	QList<QByteArray> *pPlainTexts,
	bool *pClosed,
	QString *pErrorMessage)
{
	if (!m_bReady || pEncryptedBuffer == nullptr || pPlainTexts == nullptr
		|| pClosed == nullptr)
	{
		return false;
	}
	*pClosed = false;
	while (!pEncryptedBuffer->isEmpty())
	{
		SecBuffer buffers[4] = {
			{ static_cast<unsigned long>(pEncryptedBuffer->size()), SECBUFFER_DATA,
				pEncryptedBuffer->data() },
			{ 0, SECBUFFER_EMPTY, nullptr },
			{ 0, SECBUFFER_EMPTY, nullptr },
			{ 0, SECBUFFER_EMPTY, nullptr }
		};
		SecBufferDesc description = { SECBUFFER_VERSION, 4, buffers };
		const SECURITY_STATUS status = DecryptMessage(&m_context, &description, 0, nullptr);
		if (status == SEC_E_INCOMPLETE_MESSAGE)
			return true;
		if (status == SEC_I_CONTEXT_EXPIRED)
		{
			*pClosed = true;
			pEncryptedBuffer->clear();
			return true;
		}
		if (status != SEC_E_OK)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = SecurityStatusMessage(QStringLiteral("DecryptMessage"), status);
			return false;
		}
		SecBuffer *pData = nullptr;
		SecBuffer *pExtra = nullptr;
		for (SecBuffer &buffer : buffers)
		{
			if (buffer.BufferType == SECBUFFER_DATA)
				pData = &buffer;
			else if (buffer.BufferType == SECBUFFER_EXTRA)
				pExtra = &buffer;
		}
		if (pData != nullptr && pData->cbBuffer > 0)
		{
			pPlainTexts->append(QByteArray(static_cast<const char *>(pData->pvBuffer),
				static_cast<int>(pData->cbBuffer)));
		}
		if (pExtra != nullptr)
			pEncryptedBuffer->remove(0, pEncryptedBuffer->size() - pExtra->cbBuffer);
		else
			pEncryptedBuffer->clear();
	}
	return true;
}

bool KSchannelTlsEngine::peerIdentity(const QString &strSourceAddress,
	KTlsPeerIdentity *pIdentity,
	QString *pErrorMessage) const
{
	if (!m_bReady || pIdentity == nullptr)
		return false;
	PCCERT_CONTEXT pPeerCertificate = nullptr;
	SECURITY_STATUS status = QueryContextAttributesW(
		const_cast<PCtxtHandle>(&m_context),
		SECPKG_ATTR_REMOTE_CERT_CONTEXT, &pPeerCertificate);
	if (status != SEC_E_OK || pPeerCertificate == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("The peer did not provide a device certificate");
		return false;
	}
	QString strDeviceId;
	const bool bValid = ValidatePeerCertificate(pPeerCertificate,
		&strDeviceId, pErrorMessage);
	QByteArray spkiDer;
	if (!bValid || !EncodeSpki(pPeerCertificate, &spkiDer, pErrorMessage))
	{
		CertFreeCertificateContext(pPeerCertificate);
		return false;
	}
	SecPkgContext_CipherInfo cipherInfo = {};
	cipherInfo.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
	status = QueryContextAttributesW(const_cast<PCtxtHandle>(&m_context),
		SECPKG_ATTR_CIPHER_INFO, &cipherInfo);
	if (status != SEC_E_OK)
	{
		CertFreeCertificateContext(pPeerCertificate);
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to query the negotiated TLS cipher");
		return false;
	}
	KTlsPeerIdentity identity;
	identity.strDeviceId = strDeviceId;
	identity.strSourceAddress = strSourceAddress;
	identity.spkiSha256 = QCryptographicHash::hash(spkiDer, QCryptographicHash::Sha256);
	const QByteArray certificateDer(
		reinterpret_cast<const char *>(pPeerCertificate->pbCertEncoded),
		static_cast<int>(pPeerCertificate->cbCertEncoded));
	identity.certificateSha256 = QCryptographicHash::hash(
		certificateDer, QCryptographicHash::Sha256);
	identity.validFromUtc = DateTimeFromFileTime(pPeerCertificate->pCertInfo->NotBefore);
	identity.validToUtc = DateTimeFromFileTime(pPeerCertificate->pCertInfo->NotAfter);
	identity.strTlsProtocol = ProtocolName(cipherInfo.dwProtocol);
	identity.strCipherSuite = QString::fromWCharArray(cipherInfo.szCipherSuite);
	CertFreeCertificateContext(pPeerCertificate);
	if (!identity.isValid())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("The peer TLS identity is incomplete");
		return false;
	}
	*pIdentity = identity;
	return true;
}

bool KSchannelTlsEngine::exportKeyingMaterial(const QByteArray &label,
	const QByteArray &context,
	int nLength,
	QByteArray *pKeyingMaterial,
	QString *pErrorMessage)
{
	if (!m_bReady || !m_bContextValid || pKeyingMaterial == nullptr
		|| label.isEmpty() || label.contains('\0')
		|| label.size() >= USHRT_MAX || context.size() > USHRT_MAX
		|| nLength <= 0 || nLength > 64)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("TLS keying material request is invalid");
		return false;
	}

	QByteArray terminatedLabel = label;
	terminatedLabel.append('\0');
	SecPkgContext_KeyingMaterialInfo info = {};
	info.cbLabel = static_cast<WORD>(terminatedLabel.size());
	info.pszLabel = terminatedLabel.data();
	info.cbContextValue = static_cast<WORD>(context.size());
	info.pbContextValue = context.isEmpty()
		? nullptr : reinterpret_cast<PBYTE>(const_cast<char *>(context.constData()));
	info.cbKeyingMaterial = static_cast<DWORD>(nLength);
	SECURITY_STATUS status = SetContextAttributesW(&m_context,
		SECPKG_ATTR_KEYING_MATERIAL_INFO, &info, sizeof(info));
	if (status != SEC_E_OK)
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = QStringLiteral("Unable to configure TLS keying material (0x%1)")
				.arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
		}
		return false;
	}

	SecPkgContext_KeyingMaterial material = {};
	status = QueryContextAttributesW(&m_context,
		SECPKG_ATTR_KEYING_MATERIAL, &material);
	if (status != SEC_E_OK || material.pbKeyingMaterial == nullptr
		|| material.cbKeyingMaterial != static_cast<DWORD>(nLength))
	{
		if (material.pbKeyingMaterial != nullptr)
			FreeContextBuffer(material.pbKeyingMaterial);
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = QStringLiteral("Unable to export TLS keying material (0x%1)")
				.arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
		}
		return false;
	}

	*pKeyingMaterial = QByteArray(
		reinterpret_cast<const char *>(material.pbKeyingMaterial),
		static_cast<qsizetype>(material.cbKeyingMaterial));
	SecureZeroMemory(material.pbKeyingMaterial, material.cbKeyingMaterial);
	FreeContextBuffer(material.pbKeyingMaterial);
	return true;
}

bool KSchannelTlsEngine::isReady() const
{
	return m_bReady;
}

void KSchannelTlsEngine::clear()
{
	if (m_bContextValid)
		DeleteSecurityContext(&m_context);
	if (m_bCredentialValid)
		FreeCredentialsHandle(&m_credential);
	if (m_pLocalCertificate != nullptr)
		CertFreeCertificateContext(m_pLocalCertificate);
	m_credential = {};
	m_context = {};
	m_pLocalCertificate = nullptr;
	m_streamSizes = {};
	m_bServer = false;
	m_bCredentialValid = false;
	m_bContextValid = false;
	m_bReady = false;
}
