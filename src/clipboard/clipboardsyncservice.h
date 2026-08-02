#ifndef _WINREMOTECONTROL_CLIPBOARD_CLIPBOARDSYNCSERVICE_H_
#define _WINREMOTECONTROL_CLIPBOARD_CLIPBOARDSYNCSERVICE_H_

#include "core/protocol/clipboardmessage.h"

#include <QtCore/QObject>
#include <QtCore/QQueue>
#include <QtCore/QSet>
#include <QtCore/QString>

#include <memory>

class KClipboardAdapter;
class KSessionController;
class QTimer;

class KClipboardSyncService final : public QObject
{
	Q_OBJECT

public:
	explicit KClipboardSyncService(std::unique_ptr<KClipboardAdapter> spClipboardAdapter,
		KSessionController *pSessionController,
		QObject *pParent = nullptr);
	~KClipboardSyncService() override;

	KClipboardSyncService(const KClipboardSyncService &) = delete;
	KClipboardSyncService &operator=(const KClipboardSyncService &) = delete;

public slots:
	void setEnabled(bool bEnabled);
	void requestState();
	void shutdown();

signals:
	void syncStateChanged(bool bEnabled,
		bool bAvailable,
		bool bActive,
		const QString &strStatus);
	void syncError(const QString &strMessage);

private:
	void handleLocalTextChanged(const QString &strText);
	void handleRemoteMessage(const KClipboardMessage &message);
	void handleChannelChanged(bool bOpen);
	void handleSessionStateChanged(const QString &strState);
	void retryPendingApply();
	void applyRemoteMessage(const KClipboardMessage &message, int nAttempt);
	void rememberMessageId(const QString &strMessageId);
	void resetSession();
	void emitState();
	void sendReadyIfNeeded();
	bool isActive() const;

	std::unique_ptr<KClipboardAdapter> m_spClipboardAdapter;
	KSessionController *m_pSessionController = nullptr;
	QTimer *m_pRetryTimer = nullptr;
	bool m_bEnabled = true;
	bool m_bChannelOpen = false;
	bool m_bPeerReady = false;
	bool m_bReadySent = false;
	bool m_bSessionEstablished = false;
	QString m_strSessionState;
	QString m_strSuppressedText;
	KClipboardMessage m_pendingMessage;
	int m_nPendingAttempt = 0;
	QQueue<QString> m_recentMessageIds;
	QSet<QString> m_recentMessageIdSet;
};

#endif // _WINREMOTECONTROL_CLIPBOARD_CLIPBOARDSYNCSERVICE_H_
