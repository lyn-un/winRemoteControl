#include "adapters/windows/file_transfer/windowsfilesystemadapter.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QByteArrayView>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QSet>
#include <QtCore/QScopeGuard>
#include <QtCore/QTimeZone>
#include <QtCore/QUuid>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace
{
	constexpr int kMaximumReadChunkBytes = 4 * 1024 * 1024;
	constexpr int kMaximumDirectoryTreeEntries = 50000;
	constexpr int kSha256Bytes = 32;
	constexpr int kMaximumFileNameLength = 255;
	constexpr int kMaximumKeepBothAttempts = 10000;
	constexpr qint64 kWindowsEpochOffsetMilliseconds = 11644473600000LL;

	bool Fail(const QString &strMessage, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strMessage;
		return false;
	}

	void ClearError(QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			pErrorMessage->clear();
	}

	QString WindowsError(const QString &strOperation, DWORD nError)
	{
		wchar_t *pMessage = nullptr;
		const DWORD nLength = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER
				| FORMAT_MESSAGE_FROM_SYSTEM
				| FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			nError,
			0,
			reinterpret_cast<wchar_t *>(&pMessage),
			0,
			nullptr);
		QString strSystemMessage;
		if (nLength > 0 && pMessage != nullptr)
			strSystemMessage = QString::fromWCharArray(pMessage, static_cast<int>(nLength)).trimmed();
		if (pMessage != nullptr)
			LocalFree(pMessage);
		if (strSystemMessage.isEmpty())
			return QStringLiteral("%1 failed (0x%2)")
				.arg(strOperation)
				.arg(nError, 8, 16, QLatin1Char('0'));
		return QStringLiteral("%1 failed (0x%2): %3")
			.arg(strOperation)
			.arg(nError, 8, 16, QLatin1Char('0'))
			.arg(strSystemMessage);
	}

	class KScopedHandle
	{
	public:
		KScopedHandle() = default;
		explicit KScopedHandle(HANDLE hHandle)
			: m_hHandle(hHandle)
		{
		}

		~KScopedHandle()
		{
			close();
		}

		KScopedHandle(const KScopedHandle &) = delete;
		KScopedHandle &operator=(const KScopedHandle &) = delete;

		KScopedHandle(KScopedHandle &&other) noexcept
			: m_hHandle(std::exchange(other.m_hHandle, INVALID_HANDLE_VALUE))
		{
		}

		KScopedHandle &operator=(KScopedHandle &&other) noexcept
		{
			if (this == &other)
				return *this;
			close();
			m_hHandle = std::exchange(other.m_hHandle, INVALID_HANDLE_VALUE);
			return *this;
		}

		HANDLE handle() const
		{
			return m_hHandle;
		}

		bool isValid() const
		{
			return m_hHandle != nullptr && m_hHandle != INVALID_HANDLE_VALUE;
		}

		bool close()
		{
			if (!isValid())
				return true;
			const HANDLE hHandle = m_hHandle;
			m_hHandle = INVALID_HANDLE_VALUE;
			return CloseHandle(hHandle) != FALSE;
		}

	private:
		HANDLE m_hHandle = INVALID_HANDLE_VALUE;
	};

	struct KPathIdentity
	{
		DWORD nVolumeSerialNumber = 0;
		quint64 nFileIndex = 0;
	};

	struct KValidatedPath
	{
		QString strPath;
		DWORD nAttributes = 0;
		quint64 nSize = 0;
		qint64 nLastWriteTime = 0;
		KPathIdentity identity;
	};

	bool IsSameIdentity(const KPathIdentity &left, const KPathIdentity &right)
	{
		return left.nVolumeSerialNumber == right.nVolumeSerialNumber
			&& left.nFileIndex == right.nFileIndex;
	}

	QString NativeApiPath(const QString &strPath)
	{
		return QStringLiteral("\\\\?\\%1").arg(strPath);
	}

	bool IsDriveLetter(QChar character)
	{
		return (character >= QLatin1Char('A') && character <= QLatin1Char('Z'))
			|| (character >= QLatin1Char('a') && character <= QLatin1Char('z'));
	}

	bool IsReservedDeviceName(const QString &strName)
	{
		const int nDotIndex = strName.indexOf(QLatin1Char('.'));
		const QString strStem = (nDotIndex < 0 ? strName : strName.left(nDotIndex)).toUpper();
		if (strStem == QStringLiteral("CON")
			|| strStem == QStringLiteral("PRN")
			|| strStem == QStringLiteral("AUX")
			|| strStem == QStringLiteral("NUL"))
		{
			return true;
		}
		if (strStem.size() == 4
			&& (strStem.startsWith(QStringLiteral("COM"))
				|| strStem.startsWith(QStringLiteral("LPT"))))
		{
			return strStem.at(3) >= QLatin1Char('1')
				&& strStem.at(3) <= QLatin1Char('9');
		}
		return false;
	}

	bool IsSafeFileName(const QString &strName, QString *pErrorMessage)
	{
		if (strName.isEmpty() || strName == QStringLiteral(".")
			|| strName == QStringLiteral(".."))
		{
			return Fail(QStringLiteral("File name is empty or reserved"), pErrorMessage);
		}
		if (strName.size() > kMaximumFileNameLength)
			return Fail(QStringLiteral("File name exceeds the Windows length limit"), pErrorMessage);
		if (strName.endsWith(QLatin1Char('.')) || strName.endsWith(QLatin1Char(' ')))
			return Fail(QStringLiteral("File name has an unsafe trailing character"), pErrorMessage);
		if (strName.endsWith(QStringLiteral(".wrc-part"), Qt::CaseInsensitive))
			return Fail(QStringLiteral("File name uses the reserved transfer suffix"), pErrorMessage);
		for (QChar character : strName)
		{
			if (character.unicode() < 32
				|| QStringLiteral("<>:\"/\\|?*").contains(character))
			{
				return Fail(QStringLiteral("File name contains a forbidden Windows character"),
					pErrorMessage);
			}
		}
		if (IsReservedDeviceName(strName))
			return Fail(QStringLiteral("File name is a reserved Windows device name"), pErrorMessage);
		return true;
	}

	bool NormalizeDrivePath(const QString &strInput,
		QString *pNormalizedPath,
		QString *pErrorMessage)
	{
		if (pNormalizedPath == nullptr)
			return Fail(QStringLiteral("Canonical path output is null"), pErrorMessage);
		if (strInput.contains(QChar(u'\0')))
			return Fail(QStringLiteral("Path contains a null character"), pErrorMessage);

		QString strPath = strInput;
		strPath.replace(QLatin1Char('/'), QLatin1Char('\\'));
		if (strPath.size() < 3
			|| !IsDriveLetter(strPath.at(0))
			|| strPath.at(1) != QLatin1Char(':')
			|| strPath.at(2) != QLatin1Char('\\'))
		{
			return Fail(QStringLiteral("Path must be an absolute drive-letter path"), pErrorMessage);
		}
		if (strPath.indexOf(QLatin1Char(':'), 2) >= 0)
			return Fail(QStringLiteral("Alternate data streams are not allowed"), pErrorMessage);

		while (strPath.size() > 3 && strPath.endsWith(QLatin1Char('\\')))
			strPath.chop(1);
		if (strPath.size() > 3)
		{
			const QStringList components = strPath.mid(3).split(
				QLatin1Char('\\'), Qt::KeepEmptyParts);
			for (const QString &strComponent : components)
			{
				if (strComponent.isEmpty())
					return Fail(QStringLiteral("Path contains an empty component"), pErrorMessage);
				if (strComponent == QStringLiteral(".") || strComponent == QStringLiteral(".."))
					return Fail(QStringLiteral("Path traversal components are not allowed"),
						pErrorMessage);
				if (strComponent.endsWith(QLatin1Char('.'))
					|| strComponent.endsWith(QLatin1Char(' ')))
				{
					return Fail(QStringLiteral("Path contains an unsafe trailing character"),
						pErrorMessage);
				}
				if (IsReservedDeviceName(strComponent))
					return Fail(QStringLiteral("Path contains a reserved device name"),
						pErrorMessage);
			}
		}

		const DWORD nRequiredLength = GetFullPathNameW(
			reinterpret_cast<LPCWSTR>(strPath.utf16()), 0, nullptr, nullptr);
		if (nRequiredLength == 0)
			return Fail(WindowsError(QStringLiteral("GetFullPathNameW"), GetLastError()),
				pErrorMessage);
		std::vector<wchar_t> buffer(static_cast<size_t>(nRequiredLength) + 1);
		const DWORD nWritten = GetFullPathNameW(
			reinterpret_cast<LPCWSTR>(strPath.utf16()),
			static_cast<DWORD>(buffer.size()),
			buffer.data(),
			nullptr);
		if (nWritten == 0 || nWritten >= buffer.size())
			return Fail(WindowsError(QStringLiteral("GetFullPathNameW"), GetLastError()),
				pErrorMessage);

		QString strNormalized = QString::fromWCharArray(buffer.data(), static_cast<int>(nWritten));
		strNormalized.replace(QLatin1Char('/'), QLatin1Char('\\'));
		while (strNormalized.size() > 3 && strNormalized.endsWith(QLatin1Char('\\')))
			strNormalized.chop(1);
		if (strNormalized.size() < 3
			|| !IsDriveLetter(strNormalized.at(0))
			|| strNormalized.at(1) != QLatin1Char(':')
			|| strNormalized.at(2) != QLatin1Char('\\')
			|| strNormalized.indexOf(QLatin1Char(':'), 2) >= 0)
		{
			return Fail(QStringLiteral("Canonical path is not a safe drive-letter path"),
				pErrorMessage);
		}
		strNormalized[0] = strNormalized.at(0).toUpper();
		*pNormalizedPath = strNormalized;
		return true;
	}

	bool CheckPathComponentsForReparsePoints(const QString &strPath,
		QString *pErrorMessage)
	{
		QString strCurrent = strPath.left(3);
		DWORD nAttributes = GetFileAttributesW(
			reinterpret_cast<LPCWSTR>(NativeApiPath(strCurrent).utf16()));
		if (nAttributes == INVALID_FILE_ATTRIBUTES)
			return Fail(WindowsError(QStringLiteral("GetFileAttributesW"), GetLastError()),
				pErrorMessage);
		if ((nAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			return Fail(QStringLiteral("Reparse points are not allowed"), pErrorMessage);

		const QStringList components = strPath.mid(3).split(QLatin1Char('\\'), Qt::SkipEmptyParts);
		for (const QString &strComponent : components)
		{
			if (!strCurrent.endsWith(QLatin1Char('\\')))
				strCurrent.append(QLatin1Char('\\'));
			strCurrent.append(strComponent);
			nAttributes = GetFileAttributesW(
				reinterpret_cast<LPCWSTR>(NativeApiPath(strCurrent).utf16()));
			if (nAttributes == INVALID_FILE_ATTRIBUTES)
				return Fail(WindowsError(QStringLiteral("GetFileAttributesW"), GetLastError()),
					pErrorMessage);
			if ((nAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				return Fail(QStringLiteral("Reparse points are not allowed"), pErrorMessage);
		}
		return true;
	}

	bool FinalPathFromHandle(HANDLE hHandle,
		QString *pFinalPath,
		QString *pErrorMessage)
	{
		if (pFinalPath == nullptr)
			return Fail(QStringLiteral("Final path output is null"), pErrorMessage);
		DWORD nCapacity = 512;
		std::vector<wchar_t> buffer(nCapacity);
		for (;;)
		{
			const DWORD nLength = GetFinalPathNameByHandleW(hHandle,
				buffer.data(), nCapacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (nLength == 0)
				return Fail(WindowsError(QStringLiteral("GetFinalPathNameByHandleW"),
					GetLastError()), pErrorMessage);
			if (nLength < nCapacity)
			{
				QString strFinal = QString::fromWCharArray(
					buffer.data(), static_cast<int>(nLength));
				if (!strFinal.startsWith(QStringLiteral("\\\\?\\")))
					return Fail(QStringLiteral("Final path is not a DOS device path"), pErrorMessage);
				strFinal.remove(0, 4);
				return NormalizeDrivePath(strFinal, pFinalPath, pErrorMessage);
			}
			nCapacity = nLength + 1;
			buffer.resize(nCapacity);
		}
	}

	bool InformationFromHandle(HANDLE hHandle,
		KValidatedPath *pPath,
		QString *pErrorMessage)
	{
		BY_HANDLE_FILE_INFORMATION information = {};
		if (!GetFileInformationByHandle(hHandle, &information))
			return Fail(WindowsError(QStringLiteral("GetFileInformationByHandle"), GetLastError()),
				pErrorMessage);
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			return Fail(QStringLiteral("Reparse points are not allowed"), pErrorMessage);
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0)
			return Fail(QStringLiteral("Device files are not allowed"), pErrorMessage);

		ULARGE_INTEGER size = {};
		size.HighPart = information.nFileSizeHigh;
		size.LowPart = information.nFileSizeLow;
		ULARGE_INTEGER lastWriteTime = {};
		lastWriteTime.HighPart = information.ftLastWriteTime.dwHighDateTime;
		lastWriteTime.LowPart = information.ftLastWriteTime.dwLowDateTime;
		ULARGE_INTEGER fileIndex = {};
		fileIndex.HighPart = information.nFileIndexHigh;
		fileIndex.LowPart = information.nFileIndexLow;

		pPath->nAttributes = information.dwFileAttributes;
		pPath->nSize = size.QuadPart;
		pPath->nLastWriteTime = static_cast<qint64>(lastWriteTime.QuadPart);
		pPath->identity.nVolumeSerialNumber = information.dwVolumeSerialNumber;
		pPath->identity.nFileIndex = fileIndex.QuadPart;
		return true;
	}

	bool CanonicalizeExistingSafePath(const QString &strInput,
		KValidatedPath *pPath,
		QString *pErrorMessage)
	{
		if (pPath == nullptr)
			return Fail(QStringLiteral("Validated path output is null"), pErrorMessage);
		QString strNormalized;
		if (!NormalizeDrivePath(strInput, &strNormalized, pErrorMessage)
			|| !CheckPathComponentsForReparsePoints(strNormalized, pErrorMessage))
		{
			return false;
		}

		const QString strNativePath = NativeApiPath(strNormalized);
		KScopedHandle handle(CreateFileW(
			reinterpret_cast<LPCWSTR>(strNativePath.utf16()),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr));
		if (!handle.isValid())
			return Fail(WindowsError(QStringLiteral("CreateFileW"), GetLastError()), pErrorMessage);

		KValidatedPath validated;
		if (!InformationFromHandle(handle.handle(), &validated, pErrorMessage)
			|| !FinalPathFromHandle(handle.handle(), &validated.strPath, pErrorMessage)
			|| !CheckPathComponentsForReparsePoints(validated.strPath, pErrorMessage))
		{
			return false;
		}
		*pPath = validated;
		return true;
	}

	QDateTime DateTimeFromWindowsTicks(qint64 nWindowsTicks)
	{
		if (nWindowsTicks <= 0)
			return QDateTime();
		const qint64 nMilliseconds = nWindowsTicks / 10000
			- kWindowsEpochOffsetMilliseconds;
		return QDateTime::fromMSecsSinceEpoch(nMilliseconds, QTimeZone::UTC);
	}

	bool SetLastModifiedTime(HANDLE hFile,
		const QDateTime &lastModifiedUtc,
		QString *pErrorMessage)
	{
		if (!lastModifiedUtc.isValid())
			return true;
		const qint64 nMilliseconds = lastModifiedUtc.toUTC().toMSecsSinceEpoch();
		if (nMilliseconds < -kWindowsEpochOffsetMilliseconds
			|| nMilliseconds > (std::numeric_limits<qint64>::max() / 10000)
				- kWindowsEpochOffsetMilliseconds)
		{
			return Fail(QStringLiteral("Last-modified time is outside the Windows range"),
				pErrorMessage);
		}
		const quint64 nWindowsTicks = static_cast<quint64>(
			nMilliseconds + kWindowsEpochOffsetMilliseconds) * 10000ULL;
		FILETIME fileTime = {};
		fileTime.dwLowDateTime = static_cast<DWORD>(nWindowsTicks & 0xffffffffULL);
		fileTime.dwHighDateTime = static_cast<DWORD>(nWindowsTicks >> 32);
		if (SetFileTime(hFile, nullptr, nullptr, &fileTime))
			return true;
		return Fail(WindowsError(QStringLiteral("SetFileTime"), GetLastError()), pErrorMessage);
	}

	QString BaseName(const QString &strPath)
	{
		if (strPath.size() == 3 && strPath.at(1) == QLatin1Char(':'))
			return strPath.left(2);
		const int nSeparator = strPath.lastIndexOf(QLatin1Char('\\'));
		return nSeparator < 0 ? strPath : strPath.mid(nSeparator + 1);
	}

	QString ParentPath(const QString &strPath)
	{
		const int nSeparator = strPath.lastIndexOf(QLatin1Char('\\'));
		if (nSeparator <= 2)
			return strPath.left(3);
		return strPath.left(nSeparator);
	}

	QString ChildPath(const QString &strDirectory, const QString &strName)
	{
		if (strDirectory.endsWith(QLatin1Char('\\')))
			return strDirectory + strName;
		return strDirectory + QLatin1Char('\\') + strName;
	}

	QString RelativeChildPath(const QString &strDirectory, const QString &strName)
	{
		if (strDirectory.isEmpty())
			return strName;
		return strDirectory + QLatin1Char('/') + strName;
	}

	bool SafeRelativeDirectoryComponents(const QString &strRelativePath,
		QStringList *pComponents,
		QString *pErrorMessage)
	{
		if (pComponents == nullptr)
			return Fail(QStringLiteral("Relative path component output is null"), pErrorMessage);
		pComponents->clear();
		if (strRelativePath.isEmpty())
			return true;
		if (strRelativePath.size() > 30000
			|| strRelativePath.contains(QLatin1Char('\\'))
			|| strRelativePath.startsWith(QLatin1Char('/'))
			|| strRelativePath.endsWith(QLatin1Char('/')))
		{
			return Fail(QStringLiteral("Relative directory path has an unsafe form"),
				pErrorMessage);
		}
		const QStringList components = strRelativePath.split(
			QLatin1Char('/'), Qt::KeepEmptyParts);
		for (const QString &strComponent : components)
		{
			if (strComponent.isEmpty()
				|| !IsSafeFileName(strComponent, pErrorMessage))
			{
				if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
					*pErrorMessage = QStringLiteral("Relative directory path is invalid");
				return false;
			}
		}
		*pComponents = components;
		return true;
	}

	KFileListingEntryType EntryType(const KValidatedPath &path)
	{
		if (path.strPath.size() == 3 && path.strPath.at(1) == QLatin1Char(':'))
			return DriveFileListingEntryType;
		if ((path.nAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			return DirectoryFileListingEntryType;
		return RegularFileListingEntryType;
	}

	bool IsDirectoryEntryType(KFileListingEntryType type)
	{
		return type == DriveFileListingEntryType
			|| type == DirectoryFileListingEntryType;
	}

	bool IsTransferTemporaryName(const QString &strName)
	{
		return strName.endsWith(QStringLiteral(".wrc-part"), Qt::CaseInsensitive);
	}

	bool SecureDigestEquals(const QByteArray &left, const QByteArray &right)
	{
		if (left.size() != right.size())
			return false;
		unsigned char nDifference = 0;
		for (int nIndex = 0; nIndex < left.size(); ++nIndex)
		{
			nDifference |= static_cast<unsigned char>(left.at(nIndex))
				^ static_cast<unsigned char>(right.at(nIndex));
		}
		return nDifference == 0;
	}

	bool FileExists(const QString &strPath,
		bool *pExists,
		QString *pErrorMessage)
	{
		if (pExists == nullptr)
			return Fail(QStringLiteral("File-existence output is null"), pErrorMessage);
		const DWORD nAttributes = GetFileAttributesW(
			reinterpret_cast<LPCWSTR>(NativeApiPath(strPath).utf16()));
		if (nAttributes != INVALID_FILE_ATTRIBUTES)
		{
			*pExists = true;
			return true;
		}
		const DWORD nError = GetLastError();
		if (nError == ERROR_FILE_NOT_FOUND || nError == ERROR_PATH_NOT_FOUND)
		{
			*pExists = false;
			return true;
		}
		return Fail(WindowsError(QStringLiteral("GetFileAttributesW"), nError), pErrorMessage);
	}

	bool ValidateOverwriteTarget(const QString &strPath, QString *pErrorMessage)
	{
		const DWORD nAttributes = GetFileAttributesW(
			reinterpret_cast<LPCWSTR>(NativeApiPath(strPath).utf16()));
		if (nAttributes == INVALID_FILE_ATTRIBUTES)
		{
			const DWORD nError = GetLastError();
			if (nError == ERROR_FILE_NOT_FOUND || nError == ERROR_PATH_NOT_FOUND)
				return true;
			return Fail(WindowsError(QStringLiteral("GetFileAttributesW"), nError),
				pErrorMessage);
		}
		if ((nAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			return Fail(QStringLiteral("Reparse-point destinations cannot be overwritten"),
				pErrorMessage);
		if ((nAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			return Fail(QStringLiteral("Directory destinations cannot be overwritten"),
				pErrorMessage);
		return true;
	}

	bool DirectoryIsEmpty(const QString &strPath,
		bool *pEmpty,
		QString *pErrorMessage)
	{
		if (pEmpty == nullptr)
			return Fail(QStringLiteral("Directory-empty output is null"), pErrorMessage);
		*pEmpty = true;
		const QString strSearchPattern = ChildPath(strPath, QStringLiteral("*"));
		WIN32_FIND_DATAW findData = {};
		HANDLE hFind = FindFirstFileExW(
			reinterpret_cast<LPCWSTR>(NativeApiPath(strSearchPattern).utf16()),
			FindExInfoBasic,
			&findData,
			FindExSearchNameMatch,
			nullptr,
			0);
		if (hFind == INVALID_HANDLE_VALUE)
			return Fail(WindowsError(QStringLiteral("FindFirstFileExW"), GetLastError()),
				pErrorMessage);
		do
		{
			const QString strName = QString::fromWCharArray(findData.cFileName);
			if (strName != QStringLiteral(".") && strName != QStringLiteral(".."))
			{
				*pEmpty = false;
				FindClose(hFind);
				return true;
			}
		}
		while (FindNextFileW(hFind, &findData));
		const DWORD nError = GetLastError();
		FindClose(hFind);
		if (nError == ERROR_NO_MORE_FILES)
			return true;
		return Fail(WindowsError(QStringLiteral("FindNextFileW"), nError), pErrorMessage);
	}

	QString TruncatedBaseName(const QString &strBase, int nMaximumLength)
	{
		if (strBase.size() <= nMaximumLength)
			return strBase;
		QString strTruncated = strBase.left(nMaximumLength);
		if (!strTruncated.isEmpty() && strTruncated.back().isHighSurrogate())
			strTruncated.chop(1);
		return strTruncated;
	}

	bool AvailableFileName(const QString &strDirectory,
		const QString &strRequestedName,
		KFileCollisionPolicy collisionPolicy,
		QString *pFileName,
		QString *pErrorMessage)
	{
		if (pFileName == nullptr)
			return Fail(QStringLiteral("Available file-name output is null"), pErrorMessage);
		if (collisionPolicy != RejectExistingFileCollisionPolicy
			&& collisionPolicy != OverwriteFileCollisionPolicy
			&& collisionPolicy != KeepBothFileCollisionPolicy)
		{
			return Fail(QStringLiteral("Unsupported destination collision policy"),
				pErrorMessage);
		}
		bool bExists = false;
		if (!FileExists(ChildPath(strDirectory, strRequestedName), &bExists, pErrorMessage))
			return false;
		if (collisionPolicy == OverwriteFileCollisionPolicy)
		{
			if (bExists && !ValidateOverwriteTarget(
					ChildPath(strDirectory, strRequestedName), pErrorMessage))
			{
				return false;
			}
			*pFileName = strRequestedName;
			return true;
		}
		if (!bExists)
		{
			*pFileName = strRequestedName;
			return true;
		}
		if (collisionPolicy == RejectExistingFileCollisionPolicy)
			return Fail(QStringLiteral("Destination file already exists"), pErrorMessage);
		const int nDotIndex = strRequestedName.lastIndexOf(QLatin1Char('.'));
		const bool bHasExtension = nDotIndex > 0;
		const QString strBase = bHasExtension
			? strRequestedName.left(nDotIndex) : strRequestedName;
		const QString strExtension = bHasExtension
			? strRequestedName.mid(nDotIndex) : QString();
		for (int nIndex = 1; nIndex <= kMaximumKeepBothAttempts; ++nIndex)
		{
			const QString strCounter = QStringLiteral(" (%1)").arg(nIndex);
			const int nMaximumBaseLength = kMaximumFileNameLength
				- strCounter.size() - strExtension.size();
			if (nMaximumBaseLength <= 0)
				return Fail(QStringLiteral("File extension leaves no room for a keep-both name"),
					pErrorMessage);
			const QString strCandidate = TruncatedBaseName(strBase, nMaximumBaseLength)
				+ strCounter + strExtension;
			if (!FileExists(ChildPath(strDirectory, strCandidate), &bExists, pErrorMessage))
				return false;
			if (!bExists)
			{
				*pFileName = strCandidate;
				return true;
			}
		}
		return Fail(QStringLiteral("Unable to allocate a keep-both file name"), pErrorMessage);
	}
}

struct KFileEntryReference
{
	QString strPath;
	KFileListingEntryType type = InvalidFileListingEntryType;
	KPathIdentity identity;
};

struct KFileSourceState
{
	KScopedHandle handle;
	KFileSourceSnapshot snapshot;
	KPathIdentity identity;
	qint64 nLastWriteTime = 0;
	QMutex mutex;
};

struct KFileWriteState
{
	enum KPhase
	{
		WritablePhase,
		CancellationRequestedPhase,
		CommittingPhase,
		DonePhase
	};

	KScopedHandle handle;
	QString strDirectoryOpaqueId;
	QString strDirectoryPath;
	QString strTemporaryPath;
	QString strRequestedFileName;
	QString strProposedFileName;
	quint64 nExpectedSize = 0;
	quint64 nBytesWritten = 0;
	QDateTime lastModifiedUtc;
	KFileCollisionPolicy collisionPolicy = RejectExistingFileCollisionPolicy;
	QCryptographicHash hash{QCryptographicHash::Sha256};
	bool bFailed = false;
	std::atomic<KPhase> phase = WritablePhase;
	QMutex mutex;

	~KFileWriteState()
	{
		handle.close();
		if (!strTemporaryPath.isEmpty())
		{
			DeleteFileW(reinterpret_cast<LPCWSTR>(
				NativeApiPath(strTemporaryPath).utf16()));
		}
	}
};

struct KCreatedDirectoryReference
{
	QString strDirectoryOpaqueId;
	QString strPath;
	KPathIdentity identity;
};

class KWindowsFileSystemAdapterPrivate
{
public:
	QString rememberEntry(const KValidatedPath &path, KFileListingEntryType type)
	{
		const QString strPathKey = path.strPath.toCaseFolded();
		QMutexLocker<QMutex> locker(&m_mutex);
		const auto tokenIterator = m_tokenByPath.constFind(strPathKey);
		if (tokenIterator != m_tokenByPath.constEnd())
		{
			const auto referenceIterator = m_entryByToken.constFind(tokenIterator.value());
			if (referenceIterator != m_entryByToken.constEnd()
				&& referenceIterator->type == type
				&& IsSameIdentity(referenceIterator->identity, path.identity))
			{
				return tokenIterator.value();
			}
			m_entryByToken.remove(tokenIterator.value());
			m_tokenByPath.remove(strPathKey);
		}

		const QString strToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
		KFileEntryReference reference;
		reference.strPath = path.strPath;
		reference.type = type;
		reference.identity = path.identity;
		m_entryByToken.insert(strToken, reference);
		m_tokenByPath.insert(strPathKey, strToken);
		return strToken;
	}

	bool resolveEntry(const QString &strOpaqueId,
		KValidatedPath *pPath,
		KFileListingEntryType *pType,
		QString *pErrorMessage) const
	{
		KFileEntryReference reference;
		{
			QMutexLocker<QMutex> locker(&m_mutex);
			const auto iterator = m_entryByToken.constFind(strOpaqueId);
			if (iterator == m_entryByToken.constEnd())
				return Fail(QStringLiteral("Unknown or expired file-system entry"), pErrorMessage);
			reference = iterator.value();
		}

		KValidatedPath path;
		if (!CanonicalizeExistingSafePath(reference.strPath, &path, pErrorMessage))
			return false;
		if (!IsSameIdentity(reference.identity, path.identity)
			|| EntryType(path) != reference.type)
		{
			return Fail(QStringLiteral("File-system entry changed after it was listed"),
				pErrorMessage);
		}
		if (pPath != nullptr)
			*pPath = path;
		if (pType != nullptr)
			*pType = reference.type;
		return true;
	}

	std::shared_ptr<KFileSourceState> source(const QString &strSourceId) const
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		return m_sources.value(strSourceId);
	}

	std::shared_ptr<KFileSourceState> takeSource(const QString &strSourceId)
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		return m_sources.take(strSourceId);
	}

	void addSource(const QString &strSourceId,
		const std::shared_ptr<KFileSourceState> &spState)
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		m_sources.insert(strSourceId, spState);
	}

	std::shared_ptr<KFileWriteState> write(const QString &strWriteId) const
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		return m_writes.value(strWriteId);
	}

	std::shared_ptr<KFileWriteState> takeWrite(const QString &strWriteId)
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		return m_writes.take(strWriteId);
	}

	void addWrite(const QString &strWriteId,
		const std::shared_ptr<KFileWriteState> &spState)
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		m_writes.insert(strWriteId, spState);
	}

	QString addCreatedDirectory(const QString &strDirectoryOpaqueId,
		const KValidatedPath &path)
	{
		KCreatedDirectoryReference reference;
		reference.strDirectoryOpaqueId = strDirectoryOpaqueId;
		reference.strPath = path.strPath;
		reference.identity = path.identity;
		const QString strCleanupToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QMutexLocker<QMutex> locker(&m_mutex);
		m_createdDirectoryByToken.insert(strCleanupToken, reference);
		return strCleanupToken;
	}

	bool createdDirectory(const QString &strCleanupToken,
		KCreatedDirectoryReference *pReference) const
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		const auto iterator = m_createdDirectoryByToken.constFind(strCleanupToken);
		if (iterator == m_createdDirectoryByToken.constEnd())
			return false;
		if (pReference != nullptr)
			*pReference = iterator.value();
		return true;
	}

	void forgetCreatedDirectory(const QString &strCleanupToken)
	{
		QMutexLocker<QMutex> locker(&m_mutex);
		const KCreatedDirectoryReference reference =
			m_createdDirectoryByToken.take(strCleanupToken);
		if (reference.strDirectoryOpaqueId.isEmpty())
			return;
		const auto entryIterator = m_entryByToken.constFind(
			reference.strDirectoryOpaqueId);
		if (entryIterator == m_entryByToken.constEnd()
			|| !IsSameIdentity(entryIterator->identity, reference.identity))
		{
			return;
		}
		const QString strPathKey = entryIterator->strPath.toCaseFolded();
		m_entryByToken.remove(reference.strDirectoryOpaqueId);
		if (m_tokenByPath.value(strPathKey) == reference.strDirectoryOpaqueId)
			m_tokenByPath.remove(strPathKey);
	}

private:
	mutable QMutex m_mutex;
	QHash<QString, KFileEntryReference> m_entryByToken;
	QHash<QString, QString> m_tokenByPath;
	QHash<QString, std::shared_ptr<KFileSourceState>> m_sources;
	QHash<QString, std::shared_ptr<KFileWriteState>> m_writes;
	QHash<QString, KCreatedDirectoryReference> m_createdDirectoryByToken;
};

namespace
{
	KFileListingEntry MakeListingEntry(KWindowsFileSystemAdapterPrivate *pPrivate,
		const KValidatedPath &path,
		const QString &strName)
	{
		KFileListingEntry entry;
		entry.type = EntryType(path);
		entry.strOpaqueId = pPrivate->rememberEntry(path, entry.type);
		entry.strName = strName;
		entry.nSize = entry.type == RegularFileListingEntryType ? path.nSize : 0;
		entry.lastModifiedUtc = DateTimeFromWindowsTicks(path.nLastWriteTime);
		return entry;
	}

	bool SourceMetadataMatches(const KFileSourceState &state,
		QString *pErrorMessage)
	{
		KValidatedPath current;
		if (!InformationFromHandle(state.handle.handle(), &current, pErrorMessage))
			return false;
		if (!IsSameIdentity(state.identity, current.identity)
			|| state.snapshot.nSize != current.nSize
			|| state.nLastWriteTime != current.nLastWriteTime)
		{
			return Fail(QStringLiteral("Source file changed during transfer"), pErrorMessage);
		}
		return true;
	}

	bool VerifyOpenedPath(HANDLE hHandle,
		const QString &strExpectedPath,
		const KPathIdentity *pExpectedIdentity,
		KValidatedPath *pOpenedPath,
		QString *pErrorMessage)
	{
		KValidatedPath opened;
		if (!InformationFromHandle(hHandle, &opened, pErrorMessage)
			|| !FinalPathFromHandle(hHandle, &opened.strPath, pErrorMessage))
		{
			return false;
		}
		if (QString::compare(opened.strPath, strExpectedPath, Qt::CaseInsensitive) != 0)
			return Fail(QStringLiteral("Opened path does not match its canonical path"), pErrorMessage);
		if (pExpectedIdentity != nullptr
			&& !IsSameIdentity(opened.identity, *pExpectedIdentity))
		{
			return Fail(QStringLiteral("File-system entry changed while it was opened"),
				pErrorMessage);
		}
		if (pOpenedPath != nullptr)
			*pOpenedPath = opened;
		return true;
	}

	bool CleanupWriteState(KFileWriteState *pState, QString *pErrorMessage)
	{
		if (pState == nullptr)
			return Fail(QStringLiteral("Write state is null"), pErrorMessage);
		bool bSucceeded = true;
		if (!pState->handle.close())
		{
			bSucceeded = false;
			if (pErrorMessage != nullptr)
				*pErrorMessage = WindowsError(QStringLiteral("CloseHandle"), GetLastError());
		}
		if (!pState->strTemporaryPath.isEmpty())
		{
			bool bRemoved = DeleteFileW(reinterpret_cast<LPCWSTR>(
				NativeApiPath(pState->strTemporaryPath).utf16())) != FALSE;
			if (!bRemoved)
			{
				const DWORD nError = GetLastError();
				if (nError == ERROR_FILE_NOT_FOUND || nError == ERROR_PATH_NOT_FOUND)
				{
					bRemoved = true;
				}
				else
				{
					bSucceeded = false;
					if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
						*pErrorMessage = WindowsError(QStringLiteral("DeleteFileW"), nError);
				}
			}
			if (bRemoved)
				pState->strTemporaryPath.clear();
		}
		return bSucceeded;
	}
}

KWindowsFileSystemAdapter::KWindowsFileSystemAdapter()
	: m_spPrivate(std::make_unique<KWindowsFileSystemAdapterPrivate>())
{
}

KWindowsFileSystemAdapter::~KWindowsFileSystemAdapter() = default;

bool KWindowsFileSystemAdapter::listDrives(QVector<KFileListingEntry> *pEntries,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pEntries == nullptr)
		return Fail(QStringLiteral("Drive listing output is null"), pErrorMessage);
	pEntries->clear();

	const DWORD nRequiredLength = GetLogicalDriveStringsW(0, nullptr);
	if (nRequiredLength == 0)
		return Fail(WindowsError(QStringLiteral("GetLogicalDriveStringsW"), GetLastError()),
			pErrorMessage);
	std::vector<wchar_t> buffer(static_cast<size_t>(nRequiredLength) + 1);
	if (GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data()) == 0)
		return Fail(WindowsError(QStringLiteral("GetLogicalDriveStringsW"), GetLastError()),
			pErrorMessage);

	for (const wchar_t *pDrive = buffer.data(); *pDrive != L'\0'; pDrive += wcslen(pDrive) + 1)
	{
		const QString strDrive = QString::fromWCharArray(pDrive);
		if (strDrive.size() != 3 || !IsDriveLetter(strDrive.at(0))
			|| strDrive.at(1) != QLatin1Char(':'))
		{
			continue;
		}
		const UINT nDriveType = GetDriveTypeW(pDrive);
		if (nDriveType == DRIVE_UNKNOWN || nDriveType == DRIVE_NO_ROOT_DIR)
			continue;

		KValidatedPath path;
		QString strIgnoredError;
		if (!CanonicalizeExistingSafePath(strDrive, &path, &strIgnoredError)
			|| EntryType(path) != DriveFileListingEntryType)
		{
			continue;
		}
		pEntries->append(MakeListingEntry(m_spPrivate.get(), path, path.strPath.left(2)));
	}
	std::sort(pEntries->begin(), pEntries->end(),
		[](const KFileListingEntry &left, const KFileListingEntry &right)
		{
			return QString::compare(left.strName, right.strName, Qt::CaseInsensitive) < 0;
		});
	if (pEntries->isEmpty())
		return Fail(QStringLiteral("No accessible drive-letter roots were found"), pErrorMessage);
	return true;
}

bool KWindowsFileSystemAdapter::listDirectory(const QString &strDirectoryOpaqueId,
	QVector<KFileListingEntry> *pEntries,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pEntries == nullptr)
		return Fail(QStringLiteral("Directory listing output is null"), pErrorMessage);
	pEntries->clear();

	KValidatedPath directory;
	KFileListingEntryType type = InvalidFileListingEntryType;
	if (!m_spPrivate->resolveEntry(strDirectoryOpaqueId, &directory, &type, pErrorMessage))
		return false;
	if (!IsDirectoryEntryType(type))
		return Fail(QStringLiteral("File-system entry is not a directory"), pErrorMessage);

	const QString strSearchPattern = ChildPath(directory.strPath, QStringLiteral("*"));
	WIN32_FIND_DATAW findData = {};
	HANDLE hFind = FindFirstFileExW(
		reinterpret_cast<LPCWSTR>(NativeApiPath(strSearchPattern).utf16()),
		FindExInfoBasic,
		&findData,
		FindExSearchNameMatch,
		nullptr,
		FIND_FIRST_EX_LARGE_FETCH);
	if (hFind == INVALID_HANDLE_VALUE)
		return Fail(WindowsError(QStringLiteral("FindFirstFileExW"), GetLastError()), pErrorMessage);

	bool bEnumerationSucceeded = true;
	do
	{
		const QString strName = QString::fromWCharArray(findData.cFileName);
		if (strName == QStringLiteral(".") || strName == QStringLiteral("..")
			|| IsTransferTemporaryName(strName)
			|| (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
			|| (findData.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0)
		{
			continue;
		}

		KValidatedPath child;
		QString strIgnoredError;
		if (!CanonicalizeExistingSafePath(ChildPath(directory.strPath, strName),
				&child, &strIgnoredError))
		{
			continue;
		}
		pEntries->append(MakeListingEntry(m_spPrivate.get(), child, strName));
	}
	while (FindNextFileW(hFind, &findData));
	const DWORD nEnumerationError = GetLastError();
	if (nEnumerationError != ERROR_NO_MORE_FILES)
	{
		bEnumerationSucceeded = false;
		Fail(WindowsError(QStringLiteral("FindNextFileW"), nEnumerationError), pErrorMessage);
	}
	FindClose(hFind);
	if (!bEnumerationSucceeded)
	{
		pEntries->clear();
		return false;
	}

	std::sort(pEntries->begin(), pEntries->end(),
		[](const KFileListingEntry &left, const KFileListingEntry &right)
		{
			const bool bLeftDirectory = IsDirectoryEntryType(left.type);
			const bool bRightDirectory = IsDirectoryEntryType(right.type);
			if (bLeftDirectory != bRightDirectory)
				return bLeftDirectory;
			return QString::compare(left.strName, right.strName, Qt::CaseInsensitive) < 0;
		});
	return true;
}

bool KWindowsFileSystemAdapter::expandDirectoryTree(const QString &strDirectoryOpaqueId,
	int nMaximumEntries,
	KFileTreeExpansion *pExpansion,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pExpansion == nullptr)
		return Fail(QStringLiteral("Directory expansion output is null"), pErrorMessage);
	*pExpansion = KFileTreeExpansion();
	if (nMaximumEntries <= 0 || nMaximumEntries > kMaximumDirectoryTreeEntries)
	{
		return Fail(QStringLiteral("Directory expansion limit must be between 1 and 50000"),
			pErrorMessage);
	}

	KValidatedPath rootDirectory;
	KFileListingEntryType rootType = InvalidFileListingEntryType;
	if (!m_spPrivate->resolveEntry(strDirectoryOpaqueId,
			&rootDirectory, &rootType, pErrorMessage))
	{
		return false;
	}
	if (!IsDirectoryEntryType(rootType))
		return Fail(QStringLiteral("File-system entry is not a directory"), pErrorMessage);

	struct KPendingDirectory
	{
		KValidatedPath path;
		QString strRelativePath;
	};
	QVector<KPendingDirectory> pendingDirectoryList;
	pendingDirectoryList.append({rootDirectory, QString()});
	int nExpandedEntryCount = 0;
	while (!pendingDirectoryList.isEmpty())
	{
		const KPendingDirectory pending = pendingDirectoryList.takeLast();
		KValidatedPath currentDirectory;
		if (!CanonicalizeExistingSafePath(pending.path.strPath,
				&currentDirectory, pErrorMessage)
			|| !IsSameIdentity(pending.path.identity, currentDirectory.identity)
			|| !IsDirectoryEntryType(EntryType(currentDirectory)))
		{
			*pExpansion = KFileTreeExpansion();
			if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
				*pErrorMessage = QStringLiteral("Directory changed during recursive expansion");
			return false;
		}
		const QString strSearchPattern = ChildPath(
			currentDirectory.strPath, QStringLiteral("*"));
		WIN32_FIND_DATAW findData = {};
		HANDLE hFind = FindFirstFileExW(
			reinterpret_cast<LPCWSTR>(NativeApiPath(strSearchPattern).utf16()),
			FindExInfoBasic,
			&findData,
			FindExSearchNameMatch,
			nullptr,
			FIND_FIRST_EX_LARGE_FETCH);
		if (hFind == INVALID_HANDLE_VALUE)
		{
			*pExpansion = KFileTreeExpansion();
			return Fail(WindowsError(QStringLiteral("FindFirstFileExW"), GetLastError()),
				pErrorMessage);
		}

		do
		{
			const QString strName = QString::fromWCharArray(findData.cFileName);
			if (strName == QStringLiteral(".") || strName == QStringLiteral(".."))
				continue;
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			{
				FindClose(hFind);
				*pExpansion = KFileTreeExpansion();
				return Fail(QStringLiteral("Directory tree contains a reparse point"),
					pErrorMessage);
			}
			if (IsTransferTemporaryName(strName))
				continue;
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0)
			{
				FindClose(hFind);
				*pExpansion = KFileTreeExpansion();
				return Fail(QStringLiteral("Directory tree contains a device entry"),
					pErrorMessage);
			}
			++nExpandedEntryCount;
			if (nExpandedEntryCount > nMaximumEntries)
			{
				FindClose(hFind);
				*pExpansion = KFileTreeExpansion();
				return Fail(QStringLiteral("Directory tree exceeds the requested entry limit"),
					pErrorMessage);
			}

			KValidatedPath child;
			if (!CanonicalizeExistingSafePath(ChildPath(currentDirectory.strPath, strName),
					&child, pErrorMessage))
			{
				FindClose(hFind);
				*pExpansion = KFileTreeExpansion();
				return false;
			}
			if (QString::compare(ParentPath(child.strPath), currentDirectory.strPath,
					Qt::CaseInsensitive) != 0)
			{
				FindClose(hFind);
				*pExpansion = KFileTreeExpansion();
				return Fail(QStringLiteral("Directory tree entry escaped its parent"),
					pErrorMessage);
			}

			const QString strRelativePath = RelativeChildPath(
				pending.strRelativePath, strName);
			if (EntryType(child) == DirectoryFileListingEntryType)
			{
				pExpansion->relativeDirectoryPathList.append(strRelativePath);
				pendingDirectoryList.append({child, strRelativePath});
			}
			else
			{
				KFileTreeFileEntry fileEntry;
				fileEntry.strRelativePath = strRelativePath;
				fileEntry.strOpaqueId = m_spPrivate->rememberEntry(
					child, RegularFileListingEntryType);
				fileEntry.nSize = child.nSize;
				fileEntry.lastModifiedUtc = DateTimeFromWindowsTicks(child.nLastWriteTime);
				pExpansion->fileList.append(fileEntry);
			}
		}
		while (FindNextFileW(hFind, &findData));
		const DWORD nEnumerationError = GetLastError();
		FindClose(hFind);
		if (nEnumerationError != ERROR_NO_MORE_FILES)
		{
			*pExpansion = KFileTreeExpansion();
			return Fail(WindowsError(QStringLiteral("FindNextFileW"), nEnumerationError),
				pErrorMessage);
		}
	}

	std::sort(pExpansion->relativeDirectoryPathList.begin(),
		pExpansion->relativeDirectoryPathList.end(),
		[](const QString &strLeft, const QString &strRight)
		{
			return QString::compare(strLeft, strRight, Qt::CaseInsensitive) < 0;
		});
	std::sort(pExpansion->fileList.begin(), pExpansion->fileList.end(),
		[](const KFileTreeFileEntry &left, const KFileTreeFileEntry &right)
		{
			return QString::compare(left.strRelativePath, right.strRelativePath,
				Qt::CaseInsensitive) < 0;
		});
	return true;
}

bool KWindowsFileSystemAdapter::createLocalReference(const QString &strAbsolutePath,
	KFileListingEntry *pEntry,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pEntry == nullptr)
		return Fail(QStringLiteral("Local reference output is null"), pErrorMessage);
	*pEntry = KFileListingEntry();

	KValidatedPath path;
	if (!CanonicalizeExistingSafePath(strAbsolutePath, &path, pErrorMessage))
		return false;
	if (EntryType(path) == RegularFileListingEntryType
		&& IsTransferTemporaryName(BaseName(path.strPath)))
	{
		return Fail(QStringLiteral("Transfer temporary files cannot be referenced"),
			pErrorMessage);
	}
	*pEntry = MakeListingEntry(m_spPrivate.get(), path, BaseName(path.strPath));
	return true;
}

bool KWindowsFileSystemAdapter::destinationExists(const QString &strDirectoryOpaqueId,
	const QString &strFileName,
	bool *pExists,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pExists == nullptr)
		return Fail(QStringLiteral("Destination-existence output is null"), pErrorMessage);
	*pExists = false;
	if (!IsSafeFileName(strFileName, pErrorMessage))
		return false;

	KValidatedPath directory;
	KFileListingEntryType type = InvalidFileListingEntryType;
	if (!m_spPrivate->resolveEntry(strDirectoryOpaqueId, &directory, &type, pErrorMessage))
		return false;
	if (!IsDirectoryEntryType(type))
		return Fail(QStringLiteral("Destination entry is not a directory"), pErrorMessage);

	const DWORD nAttributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(
		NativeApiPath(ChildPath(directory.strPath, strFileName)).utf16()));
	if (nAttributes == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD nError = GetLastError();
		if (nError == ERROR_FILE_NOT_FOUND || nError == ERROR_PATH_NOT_FOUND)
			return true;
		return Fail(WindowsError(QStringLiteral("GetFileAttributesW"), nError),
			pErrorMessage);
	}
	if ((nAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		return Fail(QStringLiteral("Reparse-point destinations are not allowed"),
			pErrorMessage);
	if ((nAttributes & FILE_ATTRIBUTE_DEVICE) != 0)
		return Fail(QStringLiteral("Device destinations are not allowed"), pErrorMessage);
	*pExists = true;
	return true;
}

bool KWindowsFileSystemAdapter::prepareRelativeDirectories(
	const QString &strRootDirectoryOpaqueId,
	const QStringList &relativeDirectoryPathList,
	KDirectoryPreparationResult *pResult,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pResult == nullptr)
		return Fail(QStringLiteral("Directory preparation output is null"), pErrorMessage);
	*pResult = KDirectoryPreparationResult();
	if (relativeDirectoryPathList.size() > kMaximumDirectoryTreeEntries)
		return Fail(QStringLiteral("Directory preparation exceeds 50000 paths"), pErrorMessage);

	KValidatedPath rootDirectory;
	KFileListingEntryType rootType = InvalidFileListingEntryType;
	if (!m_spPrivate->resolveEntry(strRootDirectoryOpaqueId,
			&rootDirectory, &rootType, pErrorMessage))
	{
		return false;
	}
	if (!IsDirectoryEntryType(rootType))
		return Fail(QStringLiteral("Destination root is not a directory"), pErrorMessage);

	struct KRequestedDirectory
	{
		QString strRelativePath;
		QStringList componentList;
	};
	QVector<KRequestedDirectory> requestedDirectoryList;
	QSet<QString> relativePathKeys;
	for (const QString &strRelativePath : relativeDirectoryPathList)
	{
		QStringList componentList;
		if (!SafeRelativeDirectoryComponents(strRelativePath,
				&componentList, pErrorMessage))
		{
			return false;
		}
		const QString strNormalizedPath = componentList.join(QLatin1Char('/'));
		const QString strPathKey = strNormalizedPath.toCaseFolded();
		if (relativePathKeys.contains(strPathKey))
			continue;
		relativePathKeys.insert(strPathKey);
		requestedDirectoryList.append({strNormalizedPath, componentList});
	}
	std::sort(requestedDirectoryList.begin(), requestedDirectoryList.end(),
		[](const KRequestedDirectory &left, const KRequestedDirectory &right)
		{
			if (left.componentList.size() != right.componentList.size())
				return left.componentList.size() < right.componentList.size();
			return QString::compare(left.strRelativePath, right.strRelativePath,
				Qt::CaseInsensitive) < 0;
		});

	const auto rollbackFailure = [this, pResult, pErrorMessage](
		const QString &strOriginalError) -> bool
	{
		QStringList cleanupTokenList;
		for (const KCreatedDirectoryToken &created : pResult->createdDirectoryList)
			cleanupTokenList.append(created.strCleanupToken);
		KDirectoryCleanupResult cleanupResult;
		QString strCleanupError;
		cleanupCreatedDirectories(cleanupTokenList, &cleanupResult, &strCleanupError);
		*pResult = KDirectoryPreparationResult();
		QString strCombinedError = strOriginalError;
		if (!strCleanupError.isEmpty())
		{
			strCombinedError.append(QStringLiteral("; directory rollback failed: %1")
				.arg(strCleanupError));
		}
		return Fail(strCombinedError, pErrorMessage);
	};

	for (const KRequestedDirectory &requested : requestedDirectoryList)
	{
		KValidatedPath currentDirectory = rootDirectory;
		QString strCurrentOpaqueId = strRootDirectoryOpaqueId;
		QString strCurrentRelativePath;
		for (const QString &strComponent : requested.componentList)
		{
			KValidatedPath revalidatedParent;
			QString strParentValidationError;
			if (!CanonicalizeExistingSafePath(currentDirectory.strPath,
					&revalidatedParent, &strParentValidationError)
				|| !IsSameIdentity(currentDirectory.identity, revalidatedParent.identity)
				|| !IsDirectoryEntryType(EntryType(revalidatedParent)))
			{
				if (strParentValidationError.isEmpty())
					strParentValidationError = QStringLiteral(
						"Destination directory changed during preparation");
				return rollbackFailure(strParentValidationError);
			}
			currentDirectory = revalidatedParent;
			strCurrentRelativePath = RelativeChildPath(
				strCurrentRelativePath, strComponent);
			const QString strChildPath = ChildPath(currentDirectory.strPath, strComponent);
			bool bCreated = false;
			DWORD nAttributes = GetFileAttributesW(
				reinterpret_cast<LPCWSTR>(NativeApiPath(strChildPath).utf16()));
			if (nAttributes == INVALID_FILE_ATTRIBUTES)
			{
				const DWORD nAttributeError = GetLastError();
				if (nAttributeError != ERROR_FILE_NOT_FOUND
					&& nAttributeError != ERROR_PATH_NOT_FOUND)
				{
					return rollbackFailure(WindowsError(
						QStringLiteral("GetFileAttributesW"), nAttributeError));
				}
				if (CreateDirectoryW(
						reinterpret_cast<LPCWSTR>(NativeApiPath(strChildPath).utf16()),
						nullptr))
				{
					bCreated = true;
				}
				else
				{
					const DWORD nCreateError = GetLastError();
					if (nCreateError != ERROR_ALREADY_EXISTS)
					{
						return rollbackFailure(WindowsError(
							QStringLiteral("CreateDirectoryW"), nCreateError));
					}
				}
				nAttributes = GetFileAttributesW(
					reinterpret_cast<LPCWSTR>(NativeApiPath(strChildPath).utf16()));
				if (nAttributes == INVALID_FILE_ATTRIBUTES)
				{
					const QString strFailure = WindowsError(
						QStringLiteral("GetFileAttributesW"), GetLastError());
					if (bCreated)
						RemoveDirectoryW(reinterpret_cast<LPCWSTR>(
							NativeApiPath(strChildPath).utf16()));
					return rollbackFailure(strFailure);
				}
			}
			if ((nAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
				|| (nAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				if (bCreated)
					RemoveDirectoryW(reinterpret_cast<LPCWSTR>(
						NativeApiPath(strChildPath).utf16()));
				return rollbackFailure(QStringLiteral(
					"Relative directory path contains a non-directory or reparse point"));
			}

			KValidatedPath childDirectory;
			QString strValidationError;
			if (!CanonicalizeExistingSafePath(strChildPath,
					&childDirectory, &strValidationError)
				|| EntryType(childDirectory) != DirectoryFileListingEntryType
				|| QString::compare(ParentPath(childDirectory.strPath),
					currentDirectory.strPath, Qt::CaseInsensitive) != 0)
			{
				if (bCreated)
					RemoveDirectoryW(reinterpret_cast<LPCWSTR>(
						NativeApiPath(strChildPath).utf16()));
				if (strValidationError.isEmpty())
					strValidationError = QStringLiteral("Created directory escaped its parent");
				return rollbackFailure(strValidationError);
			}

			strCurrentOpaqueId = m_spPrivate->rememberEntry(
				childDirectory, DirectoryFileListingEntryType);
			if (bCreated)
			{
				KCreatedDirectoryToken created;
				created.strRelativePath = strCurrentRelativePath;
				created.strDirectoryOpaqueId = strCurrentOpaqueId;
				created.strCleanupToken = m_spPrivate->addCreatedDirectory(
					strCurrentOpaqueId, childDirectory);
				pResult->createdDirectoryList.append(created);
			}
			currentDirectory = childDirectory;
		}

		KRelativeDirectoryEntry directoryEntry;
		directoryEntry.strRelativePath = requested.strRelativePath;
		directoryEntry.strDirectoryOpaqueId = strCurrentOpaqueId;
		pResult->directoryList.append(directoryEntry);
	}
	return true;
}

bool KWindowsFileSystemAdapter::cleanupCreatedDirectories(
	const QStringList &cleanupTokenList,
	KDirectoryCleanupResult *pResult,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pResult == nullptr)
		return Fail(QStringLiteral("Directory cleanup output is null"), pErrorMessage);
	*pResult = KDirectoryCleanupResult();
	if (cleanupTokenList.size() > kMaximumDirectoryTreeEntries)
		return Fail(QStringLiteral("Directory cleanup exceeds 50000 tokens"), pErrorMessage);

	bool bSucceeded = true;
	QString strFirstError;
	QSet<QString> processedTokens;
	for (auto iterator = cleanupTokenList.crbegin();
		iterator != cleanupTokenList.crend(); ++iterator)
	{
		const QString strCleanupToken = *iterator;
		if (processedTokens.contains(strCleanupToken))
			continue;
		processedTokens.insert(strCleanupToken);

		KCreatedDirectoryReference reference;
		if (strCleanupToken.isEmpty()
			|| !m_spPrivate->createdDirectory(strCleanupToken, &reference))
		{
			bSucceeded = false;
			if (strFirstError.isEmpty())
				strFirstError = QStringLiteral("Unknown created-directory cleanup token");
			continue;
		}

		const DWORD nAttributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(
			NativeApiPath(reference.strPath).utf16()));
		if (nAttributes == INVALID_FILE_ATTRIBUTES)
		{
			const DWORD nError = GetLastError();
			if (nError == ERROR_FILE_NOT_FOUND || nError == ERROR_PATH_NOT_FOUND)
			{
				m_spPrivate->forgetCreatedDirectory(strCleanupToken);
				pResult->removedCleanupTokenList.prepend(strCleanupToken);
				continue;
			}
			bSucceeded = false;
			pResult->retainedCleanupTokenList.prepend(strCleanupToken);
			if (strFirstError.isEmpty())
				strFirstError = WindowsError(QStringLiteral("GetFileAttributesW"), nError);
			continue;
		}
		if ((nAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
			|| (nAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		{
			m_spPrivate->forgetCreatedDirectory(strCleanupToken);
			bSucceeded = false;
			if (strFirstError.isEmpty())
				strFirstError = QStringLiteral("Created directory changed before cleanup");
			continue;
		}

		bool bEmpty = false;
		QString strOperationError;
		if (!DirectoryIsEmpty(reference.strPath, &bEmpty, &strOperationError))
		{
			bSucceeded = false;
			pResult->retainedCleanupTokenList.prepend(strCleanupToken);
			if (strFirstError.isEmpty())
				strFirstError = strOperationError;
			continue;
		}
		if (!bEmpty)
		{
			pResult->retainedCleanupTokenList.prepend(strCleanupToken);
			continue;
		}

		KScopedHandle directoryHandle(CreateFileW(
			reinterpret_cast<LPCWSTR>(NativeApiPath(reference.strPath).utf16()),
			DELETE | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr));
		if (!directoryHandle.isValid())
		{
			bSucceeded = false;
			pResult->retainedCleanupTokenList.prepend(strCleanupToken);
			if (strFirstError.isEmpty())
				strFirstError = WindowsError(QStringLiteral("CreateFileW"), GetLastError());
			continue;
		}
		if (!VerifyOpenedPath(directoryHandle.handle(), reference.strPath,
				&reference.identity, nullptr, &strOperationError))
		{
			directoryHandle.close();
			m_spPrivate->forgetCreatedDirectory(strCleanupToken);
			bSucceeded = false;
			if (strFirstError.isEmpty())
				strFirstError = strOperationError;
			continue;
		}

		FILE_DISPOSITION_INFO disposition = {};
		disposition.DeleteFile = TRUE;
		if (!SetFileInformationByHandle(directoryHandle.handle(),
				FileDispositionInfo, &disposition, sizeof(disposition)))
		{
			const DWORD nError = GetLastError();
			directoryHandle.close();
			pResult->retainedCleanupTokenList.prepend(strCleanupToken);
			if (nError != ERROR_DIR_NOT_EMPTY)
			{
				bSucceeded = false;
				if (strFirstError.isEmpty())
					strFirstError = WindowsError(
						QStringLiteral("SetFileInformationByHandle"), nError);
			}
			continue;
		}
		if (!directoryHandle.close())
		{
			bSucceeded = false;
			if (strFirstError.isEmpty())
				strFirstError = WindowsError(QStringLiteral("CloseHandle"), GetLastError());
			continue;
		}
		m_spPrivate->forgetCreatedDirectory(strCleanupToken);
		pResult->removedCleanupTokenList.prepend(strCleanupToken);
	}

	if (!bSucceeded)
		return Fail(strFirstError, pErrorMessage);
	return true;
}

bool KWindowsFileSystemAdapter::openSourceSnapshot(const QString &strFileOpaqueId,
	KFileSourceSnapshot *pSnapshot,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pSnapshot == nullptr)
		return Fail(QStringLiteral("Source snapshot output is null"), pErrorMessage);
	*pSnapshot = KFileSourceSnapshot();

	KValidatedPath path;
	KFileListingEntryType type = InvalidFileListingEntryType;
	if (!m_spPrivate->resolveEntry(strFileOpaqueId, &path, &type, pErrorMessage))
		return false;
	if (type != RegularFileListingEntryType)
		return Fail(QStringLiteral("Source entry is not a regular file"), pErrorMessage);
	if (IsTransferTemporaryName(BaseName(path.strPath)))
		return Fail(QStringLiteral("Transfer temporary files cannot be read"), pErrorMessage);

	KScopedHandle handle(CreateFileW(
		reinterpret_cast<LPCWSTR>(NativeApiPath(path.strPath).utf16()),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr));
	if (!handle.isValid())
		return Fail(WindowsError(QStringLiteral("CreateFileW"), GetLastError()), pErrorMessage);

	KValidatedPath opened;
	if (!VerifyOpenedPath(handle.handle(), path.strPath, &path.identity,
			&opened, pErrorMessage))
	{
		return false;
	}
	if ((opened.nAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return Fail(QStringLiteral("Source entry became a directory"), pErrorMessage);

	const QString strSourceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	auto spState = std::make_shared<KFileSourceState>();
	spState->handle = std::move(handle);
	spState->snapshot.strSourceId = strSourceId;
	spState->snapshot.strName = BaseName(opened.strPath);
	spState->snapshot.nSize = opened.nSize;
	spState->snapshot.lastModifiedUtc = DateTimeFromWindowsTicks(opened.nLastWriteTime);
	spState->identity = opened.identity;
	spState->nLastWriteTime = opened.nLastWriteTime;
	m_spPrivate->addSource(strSourceId, spState);
	*pSnapshot = spState->snapshot;
	return true;
}

bool KWindowsFileSystemAdapter::readSourceChunk(const QString &strSourceId,
	quint64 nOffset,
	int nMaximumBytes,
	QByteArray *pData,
	bool *pComplete,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pData == nullptr || pComplete == nullptr)
		return Fail(QStringLiteral("Source chunk output is null"), pErrorMessage);
	pData->clear();
	*pComplete = false;
	if (nMaximumBytes <= 0 || nMaximumBytes > kMaximumReadChunkBytes)
		return Fail(QStringLiteral("Source chunk size is outside the allowed range"), pErrorMessage);

	const std::shared_ptr<KFileSourceState> spState = m_spPrivate->source(strSourceId);
	if (!spState)
		return Fail(QStringLiteral("Unknown or closed source snapshot"), pErrorMessage);
	QMutexLocker<QMutex> locker(&spState->mutex);
	if (!spState->handle.isValid())
		return Fail(QStringLiteral("Source snapshot is closed"), pErrorMessage);
	if (!SourceMetadataMatches(*spState, pErrorMessage))
		return false;
	if (nOffset > spState->snapshot.nSize)
		return Fail(QStringLiteral("Source read offset exceeds file size"), pErrorMessage);
	if (nOffset == spState->snapshot.nSize)
	{
		*pComplete = true;
		return true;
	}

	const quint64 nRemaining = spState->snapshot.nSize - nOffset;
	const DWORD nBytesToRead = static_cast<DWORD>(std::min<quint64>(
		nRemaining, static_cast<quint64>(nMaximumBytes)));
	if (nOffset > static_cast<quint64>(std::numeric_limits<qint64>::max()))
		return Fail(QStringLiteral("Source read offset exceeds Windows limits"), pErrorMessage);
	LARGE_INTEGER position = {};
	position.QuadPart = static_cast<LONGLONG>(nOffset);
	if (!SetFilePointerEx(spState->handle.handle(), position, nullptr, FILE_BEGIN))
		return Fail(WindowsError(QStringLiteral("SetFilePointerEx"), GetLastError()),
			pErrorMessage);

	pData->resize(static_cast<int>(nBytesToRead));
	DWORD nTotalBytesRead = 0;
	while (nTotalBytesRead < nBytesToRead)
	{
		DWORD nBytesRead = 0;
		if (!ReadFile(spState->handle.handle(), pData->data() + nTotalBytesRead,
				nBytesToRead - nTotalBytesRead, &nBytesRead, nullptr))
		{
			pData->clear();
			return Fail(WindowsError(QStringLiteral("ReadFile"), GetLastError()),
				pErrorMessage);
		}
		if (nBytesRead == 0)
		{
			pData->clear();
			return Fail(QStringLiteral("Source file ended before its snapshot size"),
				pErrorMessage);
		}
		nTotalBytesRead += nBytesRead;
	}
	if (!SourceMetadataMatches(*spState, pErrorMessage))
	{
		pData->clear();
		return false;
	}
	*pComplete = nOffset + nTotalBytesRead == spState->snapshot.nSize;
	return true;
}

bool KWindowsFileSystemAdapter::closeSourceSnapshot(const QString &strSourceId,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	const std::shared_ptr<KFileSourceState> spState = m_spPrivate->takeSource(strSourceId);
	if (!spState)
		return Fail(QStringLiteral("Unknown or already closed source snapshot"), pErrorMessage);
	QMutexLocker<QMutex> locker(&spState->mutex);
	if (spState->handle.close())
		return true;
	return Fail(WindowsError(QStringLiteral("CloseHandle"), GetLastError()), pErrorMessage);
}

bool KWindowsFileSystemAdapter::beginWrite(const KFileWriteRequest &request,
	KFileWriteSession *pSession,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pSession == nullptr)
		return Fail(QStringLiteral("Write session output is null"), pErrorMessage);
	*pSession = KFileWriteSession();
	if (!IsSafeFileName(request.strFileName, pErrorMessage))
		return false;
	if (request.nExpectedSize > static_cast<quint64>(std::numeric_limits<qint64>::max()))
		return Fail(QStringLiteral("Expected file size exceeds Windows limits"), pErrorMessage);
	if (request.collisionPolicy != RejectExistingFileCollisionPolicy
		&& request.collisionPolicy != OverwriteFileCollisionPolicy
		&& request.collisionPolicy != KeepBothFileCollisionPolicy)
	{
		return Fail(QStringLiteral("Unsupported destination collision policy"), pErrorMessage);
	}

	KValidatedPath directory;
	KFileListingEntryType type = InvalidFileListingEntryType;
	if (!m_spPrivate->resolveEntry(request.strDirectoryOpaqueId,
			&directory, &type, pErrorMessage))
	{
		return false;
	}
	if (!IsDirectoryEntryType(type))
		return Fail(QStringLiteral("Destination entry is not a directory"), pErrorMessage);

	QString strProposedName;
	if (!AvailableFileName(directory.strPath, request.strFileName,
			request.collisionPolicy, &strProposedName, pErrorMessage))
	{
		return false;
	}

	KScopedHandle temporaryHandle;
	QString strTemporaryPath;
	for (int nAttempt = 0; nAttempt < 16; ++nAttempt)
	{
		const QString strTemporaryName = QStringLiteral(".%1.wrc-part")
			.arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
		strTemporaryPath = ChildPath(directory.strPath, strTemporaryName);
		KScopedHandle candidate(CreateFileW(
			reinterpret_cast<LPCWSTR>(NativeApiPath(strTemporaryPath).utf16()),
			GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			nullptr,
			CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr));
		if (candidate.isValid())
		{
			temporaryHandle = std::move(candidate);
			break;
		}
		const DWORD nError = GetLastError();
		if (nError != ERROR_FILE_EXISTS && nError != ERROR_ALREADY_EXISTS)
			return Fail(WindowsError(QStringLiteral("CreateFileW"), nError), pErrorMessage);
	}
	if (!temporaryHandle.isValid())
		return Fail(QStringLiteral("Unable to allocate a transfer temporary file"), pErrorMessage);

	QString strOpenedTemporaryPath;
	if (!FinalPathFromHandle(temporaryHandle.handle(),
			&strOpenedTemporaryPath, pErrorMessage)
		|| QString::compare(ParentPath(strOpenedTemporaryPath), directory.strPath,
			Qt::CaseInsensitive) != 0)
	{
		temporaryHandle.close();
		DeleteFileW(reinterpret_cast<LPCWSTR>(NativeApiPath(strTemporaryPath).utf16()));
		if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
			*pErrorMessage = QStringLiteral("Temporary file escaped the destination directory");
		return false;
	}

	const QString strWriteId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	auto spState = std::make_shared<KFileWriteState>();
	spState->handle = std::move(temporaryHandle);
	spState->strDirectoryOpaqueId = request.strDirectoryOpaqueId;
	spState->strDirectoryPath = directory.strPath;
	spState->strTemporaryPath = strOpenedTemporaryPath;
	spState->strRequestedFileName = request.strFileName;
	spState->strProposedFileName = strProposedName;
	spState->nExpectedSize = request.nExpectedSize;
	spState->lastModifiedUtc = request.lastModifiedUtc;
	spState->collisionPolicy = request.collisionPolicy;
	m_spPrivate->addWrite(strWriteId, spState);

	pSession->strWriteId = strWriteId;
	pSession->strProposedFileName = strProposedName;
	pSession->nExpectedSize = request.nExpectedSize;
	return true;
}

bool KWindowsFileSystemAdapter::appendWriteChunk(const QString &strWriteId,
	const QByteArray &data,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	const std::shared_ptr<KFileWriteState> spState = m_spPrivate->write(strWriteId);
	if (!spState)
		return Fail(QStringLiteral("Unknown or completed write session"), pErrorMessage);
	QMutexLocker<QMutex> locker(&spState->mutex);
	if (!spState->handle.isValid() || spState->bFailed
		|| spState->phase.load() != KFileWriteState::WritablePhase)
		return Fail(QStringLiteral("Write session is not writable"), pErrorMessage);
	if (static_cast<quint64>(data.size()) > spState->nExpectedSize - spState->nBytesWritten)
		return Fail(QStringLiteral("Write chunk exceeds the declared file size"), pErrorMessage);

	qsizetype nOffset = 0;
	while (nOffset < data.size())
	{
		const DWORD nRequested = static_cast<DWORD>(std::min<qsizetype>(
			data.size() - nOffset, static_cast<qsizetype>(std::numeric_limits<DWORD>::max())));
		DWORD nWritten = 0;
		if (!WriteFile(spState->handle.handle(), data.constData() + nOffset,
				nRequested, &nWritten, nullptr))
		{
			spState->bFailed = true;
			return Fail(WindowsError(QStringLiteral("WriteFile"), GetLastError()),
				pErrorMessage);
		}
		if (nWritten == 0)
		{
			spState->bFailed = true;
			return Fail(QStringLiteral("WriteFile completed without writing data"),
				pErrorMessage);
		}
		spState->hash.addData(QByteArrayView(
			data.constData() + nOffset, static_cast<qsizetype>(nWritten)));
		nOffset += static_cast<qsizetype>(nWritten);
		spState->nBytesWritten += nWritten;
	}
	return true;
}

bool KWindowsFileSystemAdapter::finalizeWrite(const QString &strWriteId,
	const QByteArray &expectedSha256,
	KFileWriteResult *pResult,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (pResult == nullptr)
		return Fail(QStringLiteral("Write result output is null"), pErrorMessage);
	*pResult = KFileWriteResult();
	const std::shared_ptr<KFileWriteState> spState = m_spPrivate->write(strWriteId);
	if (!spState)
		return Fail(QStringLiteral("Unknown or completed write session"), pErrorMessage);
	const auto finishState = qScopeGuard([this, strWriteId, spState]()
		{
			spState->phase.store(KFileWriteState::DonePhase);
			m_spPrivate->takeWrite(strWriteId);
		});
	QMutexLocker<QMutex> locker(&spState->mutex);
	if (!spState->handle.isValid() || spState->bFailed
		|| spState->phase.load() != KFileWriteState::WritablePhase)
		return Fail(QStringLiteral("Write session cannot be finalized"), pErrorMessage);
	if (expectedSha256.size() != kSha256Bytes)
		return Fail(QStringLiteral("Expected SHA-256 digest must contain 32 bytes"), pErrorMessage);
	if (spState->nBytesWritten != spState->nExpectedSize)
		return Fail(QStringLiteral("Written byte count does not match the declared file size"),
			pErrorMessage);
	const QByteArray actualSha256 = spState->hash.result();
	if (!SecureDigestEquals(actualSha256, expectedSha256))
		return Fail(QStringLiteral("Transferred file SHA-256 mismatch"), pErrorMessage);
	if (!SetLastModifiedTime(spState->handle.handle(), spState->lastModifiedUtc,
			pErrorMessage))
	{
		return false;
	}
	if (!FlushFileBuffers(spState->handle.handle()))
		return Fail(WindowsError(QStringLiteral("FlushFileBuffers"), GetLastError()),
			pErrorMessage);

	KValidatedPath directory;
	KFileListingEntryType type = InvalidFileListingEntryType;
	if (!m_spPrivate->resolveEntry(spState->strDirectoryOpaqueId,
			&directory, &type, pErrorMessage)
		|| !IsDirectoryEntryType(type))
	{
		if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
			*pErrorMessage = QStringLiteral("Destination directory is no longer valid");
		return false;
	}
	KFileWriteState::KPhase expectedPhase = KFileWriteState::WritablePhase;
	if (!spState->phase.compare_exchange_strong(expectedPhase,
			KFileWriteState::CommittingPhase))
	{
		return Fail(QStringLiteral("Write cancellation was requested"), pErrorMessage);
	}
	// ReplaceFileW requires the verified replacement handle to be closed. The
	// temporary name remains unguessable and is never returned by directory lists.
	if (!spState->handle.close())
		return Fail(WindowsError(QStringLiteral("CloseHandle"), GetLastError()),
			pErrorMessage);

	QString strFinalName;
	QString strFinalPath;
	bool bPublished = false;
	for (int nAttempt = 0; nAttempt < kMaximumKeepBothAttempts; ++nAttempt)
	{
		if (!AvailableFileName(directory.strPath, spState->strRequestedFileName,
				spState->collisionPolicy, &strFinalName, pErrorMessage))
		{
			return false;
		}
		strFinalPath = ChildPath(directory.strPath, strFinalName);
		BOOL bAttemptPublished = FALSE;
		if (spState->collisionPolicy == OverwriteFileCollisionPolicy)
		{
			bool bDestinationExists = false;
			if (!FileExists(strFinalPath, &bDestinationExists, pErrorMessage)
				|| (bDestinationExists
					&& !ValidateOverwriteTarget(strFinalPath, pErrorMessage)))
			{
				return false;
			}
			bAttemptPublished = bDestinationExists
				? ReplaceFileW(
					reinterpret_cast<LPCWSTR>(NativeApiPath(strFinalPath).utf16()),
					reinterpret_cast<LPCWSTR>(
						NativeApiPath(spState->strTemporaryPath).utf16()),
					nullptr,
					REPLACEFILE_WRITE_THROUGH,
					nullptr,
					nullptr)
				: MoveFileExW(
					reinterpret_cast<LPCWSTR>(
						NativeApiPath(spState->strTemporaryPath).utf16()),
					reinterpret_cast<LPCWSTR>(NativeApiPath(strFinalPath).utf16()),
					MOVEFILE_WRITE_THROUGH);
		}
		else
		{
			bAttemptPublished = MoveFileExW(
				reinterpret_cast<LPCWSTR>(NativeApiPath(spState->strTemporaryPath).utf16()),
				reinterpret_cast<LPCWSTR>(NativeApiPath(strFinalPath).utf16()),
				MOVEFILE_WRITE_THROUGH);
		}
		if (bAttemptPublished)
		{
			bPublished = true;
			break;
		}
		const DWORD nError = GetLastError();
		const bool bNameCollision = nError == ERROR_FILE_EXISTS
			|| nError == ERROR_ALREADY_EXISTS;
		const bool bOverwriteTargetDisappeared = nError == ERROR_FILE_NOT_FOUND
			&& spState->collisionPolicy == OverwriteFileCollisionPolicy;
		const bool bCanRetry = (bNameCollision
				&& (spState->collisionPolicy == KeepBothFileCollisionPolicy
					|| spState->collisionPolicy == OverwriteFileCollisionPolicy))
			|| bOverwriteTargetDisappeared;
		if (!bCanRetry)
		{
			const QString strOperation = spState->collisionPolicy
				== OverwriteFileCollisionPolicy
				? QStringLiteral("ReplaceFileW") : QStringLiteral("MoveFileExW");
			return Fail(WindowsError(strOperation, nError), pErrorMessage);
		}
		strFinalPath.clear();
	}
	if (!bPublished)
		return Fail(QStringLiteral("Unable to finalize a keep-both file name"), pErrorMessage);

	KValidatedPath publishedFile;
	if (!CanonicalizeExistingSafePath(strFinalPath, &publishedFile, pErrorMessage)
		|| QString::compare(publishedFile.strPath, strFinalPath, Qt::CaseInsensitive) != 0
		|| EntryType(publishedFile) != RegularFileListingEntryType
		|| publishedFile.nSize != spState->nExpectedSize)
	{
		spState->strTemporaryPath.clear();
		if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
			*pErrorMessage = QStringLiteral("Published file failed canonical validation");
		return false;
	}
	spState->strTemporaryPath.clear();
	pResult->strFileName = strFinalName;
	pResult->nBytesWritten = spState->nBytesWritten;
	pResult->sha256 = actualSha256;
	return true;
}

bool KWindowsFileSystemAdapter::tryRequestWriteCancellation(
	const QString &strWriteId)
{
	const std::shared_ptr<KFileWriteState> spState = m_spPrivate->write(strWriteId);
	if (!spState)
		return false;
	KFileWriteState::KPhase expectedPhase = KFileWriteState::WritablePhase;
	if (spState->phase.compare_exchange_strong(expectedPhase,
			KFileWriteState::CancellationRequestedPhase))
	{
		return true;
	}
	return expectedPhase == KFileWriteState::CancellationRequestedPhase;
}

bool KWindowsFileSystemAdapter::abortWrite(const QString &strWriteId,
	QString *pErrorMessage)
{
	ClearError(pErrorMessage);
	if (!tryRequestWriteCancellation(strWriteId))
		return Fail(QStringLiteral("Write session is already committing"), pErrorMessage);
	const std::shared_ptr<KFileWriteState> spState = m_spPrivate->takeWrite(strWriteId);
	if (!spState)
		return Fail(QStringLiteral("Unknown or completed write session"), pErrorMessage);
	QMutexLocker<QMutex> locker(&spState->mutex);
	return CleanupWriteState(spState.get(), pErrorMessage);
}
