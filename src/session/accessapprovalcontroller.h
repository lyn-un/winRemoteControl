#ifndef _WINREMOTECONTROL_SESSION_ACCESSAPPROVALCONTROLLER_H_
#define _WINREMOTECONTROL_SESSION_ACCESSAPPROVALCONTROLLER_H_

#include "core/settings/applicationsettings.h"

#include <QtCore/QObject>
#include <QtCore/QString>

enum KAccessApprovalSide
{
	NoAccessApprovalSide,
	OutgoingAccessApprovalSide,
	IncomingAccessApprovalSide
};

enum KIncomingAccessDecision
{
	AskIncomingAccessDecision,
	AcceptIncomingAccessDecision,
	DenyIncomingAccessDecision,
	DisabledIncomingAccessDecision
};

struct KAccessApprovalRequest
{
	KAccessApprovalSide side = NoAccessApprovalSide;
	QString strRequestId;
	QString strDeviceName;
	QString strSourceAddress;
	quint64 nGeneration = 0;
};

class KAccessApprovalController : public QObject
{
	Q_OBJECT

public:
	explicit KAccessApprovalController(QObject *pParent = nullptr);

	KAccessApprovalController(const KAccessApprovalController &) = delete;
	KAccessApprovalController &operator=(const KAccessApprovalController &) = delete;

	KAccessApprovalRequest beginOutgoing(quint64 nGeneration, int nTimeoutMs);
	void beginIncoming(const QString &strSourceAddress,
		quint64 nGeneration,
		int nTimeoutMs);
	bool receiveIncomingRequest(const QString &strRequestId,
		const QString &strDeviceName,
		int nTimeoutMs);
	bool extendOutgoingTimeout(const QString &strRequestId, int nTimeoutMs);
	bool matches(const QString &strRequestId, quint64 nGeneration) const;
	bool hasRequestId() const;
	const KAccessApprovalRequest &request() const;
	void stopTimeout();
	KAccessApprovalRequest clear();

	static KIncomingAccessDecision incomingDecision(
		const KApplicationSettings &settings);

signals:
	void timedOut(const QString &strRequestId, quint64 nGeneration);

private:
	void startTimeout(int nTimeoutMs);

	class QTimer *m_pTimer = nullptr;
	KAccessApprovalRequest m_request;
};

#endif // _WINREMOTECONTROL_SESSION_ACCESSAPPROVALCONTROLLER_H_
