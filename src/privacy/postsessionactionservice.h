#ifndef _WINREMOTECONTROL_PRIVACY_POSTSESSIONACTIONSERVICE_H_
#define _WINREMOTECONTROL_PRIVACY_POSTSESSIONACTIONSERVICE_H_

#include "core/privacy/workstationlockadapter.h"

#include <QtCore/QObject>

#include <memory>

class KPostSessionActionService : public QObject
{
	Q_OBJECT

public:
	explicit KPostSessionActionService(
		std::unique_ptr<IKWorkstationLockAdapter> spLockAdapter,
		QObject *pParent = nullptr);

	KPostSessionActionService(const KPostSessionActionService &) = delete;
	KPostSessionActionService &operator=(const KPostSessionActionService &) = delete;

	void beginSession(quint64 nGeneration);
	void markStreaming(quint64 nGeneration);
	KPrivacyOperationResult setAction(KPostSessionAction action,
		const QString &strRequestId,
		quint64 nGeneration);
	KPrivacyOperationResult consumeAfterTeardown(quint64 nGeneration);
	KPostSessionActionStatus status() const;
	bool isSupported() const;

signals:
	void statusChanged(const KPostSessionActionStatus &status);

private:
	std::unique_ptr<IKWorkstationLockAdapter> m_spLockAdapter;
	KPostSessionActionStatus m_status;
	bool m_bEnteredStreaming = false;
	bool m_bConsumed = false;
};

#endif // _WINREMOTECONTROL_PRIVACY_POSTSESSIONACTIONSERVICE_H_
