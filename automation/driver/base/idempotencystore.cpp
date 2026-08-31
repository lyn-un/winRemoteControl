#include "automation/driver/base/idempotencystore.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>
#include <QtCore/QStringList>

namespace
{
QByteArray EncodeJsonValue(const QJsonValue &value)
{
	if (value.isObject())
	{
		const QJsonObject object = value.toObject();
		QByteArray encoded("{");
		bool bFirst = true;
		for (const QString &strKey : object.keys())
		{
			if (!bFirst)
				encoded.append(',');
			bFirst = false;
			QJsonArray keyArray;
			keyArray.append(strKey);
			const QByteArray encodedKey = QJsonDocument(keyArray)
				.toJson(QJsonDocument::Compact);
			encoded.append(encodedKey.mid(1, encodedKey.size() - 2));
			encoded.append(':');
			encoded.append(EncodeJsonValue(object.value(strKey)));
		}
		encoded.append('}');
		return encoded;
	}
	if (value.isArray())
	{
		const QJsonArray array = value.toArray();
		QByteArray encoded("[");
		for (qsizetype nIndex = 0; nIndex < array.size(); ++nIndex)
		{
			if (nIndex > 0)
				encoded.append(',');
			encoded.append(EncodeJsonValue(array.at(nIndex)));
		}
		encoded.append(']');
		return encoded;
	}

	QJsonArray wrapper;
	wrapper.append(value);
	const QByteArray encoded = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
	return encoded.mid(1, encoded.size() - 2);
}
}

const KDriverIdempotencyRecord *KDriverIdempotencyStore::record(
	const QString &strKey) const
{
	const auto iter = m_records.constFind(strKey);
	return iter == m_records.constEnd() ? nullptr : &iter.value();
}

bool KDriverIdempotencyStore::create(const QString &strKey,
	const QString &strCommandId,
	const QByteArray &canonicalArguments,
	quint64 nHostRequestId,
	qint64 nNowMs)
{
	if (strKey.isEmpty() || nHostRequestId == 0 || m_records.contains(strKey)
		|| m_keysByHostRequestId.contains(nHostRequestId))
	{
		return false;
	}
	if (m_records.size() >= kMaximumRecordCount && !removeOldestCompleted())
		return false;

	KDriverIdempotencyRecord record;
	record.strCommandId = strCommandId;
	record.canonicalArguments = canonicalArguments;
	record.nHostRequestId = nHostRequestId;
	record.nCreatedAtMs = nNowMs;
	record.nUpdatedAtMs = nNowMs;
	m_records.insert(strKey, record);
	m_keysByHostRequestId.insert(nHostRequestId, strKey);
	return true;
}

bool KDriverIdempotencyStore::markStarted(quint64 nHostRequestId, qint64 nNowMs)
{
	const auto keyIter = m_keysByHostRequestId.constFind(nHostRequestId);
	if (keyIter == m_keysByHostRequestId.constEnd())
		return false;
	const auto recordIter = m_records.find(keyIter.value());
	if (recordIter == m_records.end()
		|| recordIter->state == CompletedDriverIdempotencyState)
	{
		return false;
	}
	recordIter->state = StartedDriverIdempotencyState;
	recordIter->nUpdatedAtMs = nNowMs;
	return true;
}

bool KDriverIdempotencyStore::complete(quint64 nHostRequestId,
	const QJsonObject &response,
	int nHttpStatusCode,
	qint64 nNowMs)
{
	const auto keyIter = m_keysByHostRequestId.find(nHostRequestId);
	if (keyIter == m_keysByHostRequestId.end())
		return false;
	const QString strKey = keyIter.value();
	m_keysByHostRequestId.erase(keyIter);
	const auto recordIter = m_records.find(strKey);
	if (recordIter == m_records.end())
		return false;
	recordIter->state = CompletedDriverIdempotencyState;
	recordIter->response = response;
	recordIter->nHttpStatusCode = nHttpStatusCode;
	recordIter->nUpdatedAtMs = nNowMs;
	return true;
}

bool KDriverIdempotencyStore::removeByHostRequestId(quint64 nHostRequestId)
{
	const auto keyIter = m_keysByHostRequestId.constFind(nHostRequestId);
	if (keyIter == m_keysByHostRequestId.constEnd())
		return false;
	removeRecord(keyIter.value());
	return true;
}

KDriverIdempotencyCleanupResult KDriverIdempotencyStore::collectExpired(qint64 nNowMs)
{
	KDriverIdempotencyCleanupResult result;
	QStringList expiredKeys;
	for (auto iter = m_records.constBegin(); iter != m_records.constEnd(); ++iter)
	{
		if (nNowMs - iter->nUpdatedAtMs < kRecordTtlMs)
			continue;
		expiredKeys.append(iter.key());
		if (iter->nHostRequestId != 0)
			result.vecHostRequestIds.append(iter->nHostRequestId);
		if (iter->state == CompletedDriverIdempotencyState)
			++result.nCompletedCount;
		else if (iter->state == StartedDriverIdempotencyState)
			++result.nStartedCount;
		else
			++result.nPendingCount;
	}
	for (const QString &strKey : expiredKeys)
		removeRecord(strKey);
	return result;
}

void KDriverIdempotencyStore::clear()
{
	m_records.clear();
	m_keysByHostRequestId.clear();
}

int KDriverIdempotencyStore::size() const
{
	return m_records.size();
}

QByteArray KDriverIdempotencyStore::canonicalArguments(const QJsonObject &arguments)
{
	return EncodeJsonValue(arguments);
}

bool KDriverIdempotencyStore::removeOldestCompleted()
{
	auto oldest = m_records.end();
	for (auto iter = m_records.begin(); iter != m_records.end(); ++iter)
	{
		if (iter->state != CompletedDriverIdempotencyState)
			continue;
		if (oldest == m_records.end() || iter->nUpdatedAtMs < oldest->nUpdatedAtMs)
			oldest = iter;
	}
	if (oldest == m_records.end())
		return false;
	removeRecord(oldest.key());
	return true;
}

void KDriverIdempotencyStore::removeRecord(const QString &strKey)
{
	const auto iter = m_records.find(strKey);
	if (iter == m_records.end())
		return;
	if (iter->nHostRequestId != 0)
		m_keysByHostRequestId.remove(iter->nHostRequestId);
	m_records.erase(iter);
}
