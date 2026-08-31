#ifndef _WINREMOTECONTROL_DRIVER_IDEMPOTENCYSTORE_H_
#define _WINREMOTECONTROL_DRIVER_IDEMPOTENCYSTORE_H_

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVector>

enum KDriverIdempotencyState
{
	PendingDriverIdempotencyState,
	StartedDriverIdempotencyState,
	CompletedDriverIdempotencyState
};

struct KDriverIdempotencyRecord
{
	QString strCommandId;
	QByteArray canonicalArguments;
	KDriverIdempotencyState state = PendingDriverIdempotencyState;
	quint64 nHostRequestId = 0;
	QJsonObject response;
	int nHttpStatusCode = 200;
	qint64 nCreatedAtMs = 0;
	qint64 nUpdatedAtMs = 0;
};

struct KDriverIdempotencyCleanupResult
{
	int nCompletedCount = 0;
	int nStartedCount = 0;
	int nPendingCount = 0;
	QVector<quint64> vecHostRequestIds;
};

class KDriverIdempotencyStore
{
public:
	static constexpr qint64 kRecordTtlMs = 30 * 60 * 1000;
	static constexpr int kMaximumRecordCount = 1024;

	const KDriverIdempotencyRecord *record(const QString &strKey) const;
	bool create(const QString &strKey,
		const QString &strCommandId,
		const QByteArray &canonicalArguments,
		quint64 nHostRequestId,
		qint64 nNowMs);
	bool markStarted(quint64 nHostRequestId, qint64 nNowMs);
	bool complete(quint64 nHostRequestId,
		const QJsonObject &response,
		int nHttpStatusCode,
		qint64 nNowMs);
	bool removeByHostRequestId(quint64 nHostRequestId);
	KDriverIdempotencyCleanupResult collectExpired(qint64 nNowMs);
	void clear();
	int size() const;

	static QByteArray canonicalArguments(const QJsonObject &arguments);

private:
	bool removeOldestCompleted();
	void removeRecord(const QString &strKey);

	QHash<QString, KDriverIdempotencyRecord> m_records;
	QHash<quint64, QString> m_keysByHostRequestId;
};

#endif // _WINREMOTECONTROL_DRIVER_IDEMPOTENCYSTORE_H_
