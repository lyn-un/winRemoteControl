#ifndef _WINREMOTECONTROL_FILE_TRANSFER_FILETRANSFERSESSIONSERVICE_H_
#define _WINREMOTECONTROL_FILE_TRANSFER_FILETRANSFERSESSIONSERVICE_H_

#include "core/file_transfer/filetransferstate.h"
#include "core/protocol/filetransfercontrolmessage.h"
#include "core/protocol/filetransferdataframe.h"
#include "core/protocol/filetransferlifecyclemessage.h"
#include "core/session/sessionstatemachine.h"

#include <QtCore/QObject>
#include <QtCore/QStringList>

#include <memory>

class IKFileSystemPort;
class KFileTransferSessionServicePrivate;
class KSessionController;

class KFileTransferSessionService final : public QObject
{
	Q_OBJECT

public:
	explicit KFileTransferSessionService(std::unique_ptr<IKFileSystemPort> spFileSystem,
		KSessionController *pSessionController,
		QObject *pParent = nullptr);
	~KFileTransferSessionService() override;

	KFileTransferSessionService(const KFileTransferSessionService &) = delete;
	KFileTransferSessionService &operator=(const KFileTransferSessionService &) = delete;

	KFileTransferState state() const;
	bool isAvailable() const;
	bool hasActiveTasks() const;

public slots:
	void openCurrentFileTransfer();
	void stopCurrentFileTransfer();
	void requestSnapshot();
	void requestPaneRoots(KFileTransferPane pane);
	void navigatePane(KFileTransferPane pane,
		const QString &strListingId,
		const QString &strTargetEntryId);
	void navigatePaneByPath(KFileTransferPane pane, const QString &strAbsolutePath);
	void navigatePaneUp(KFileTransferPane pane, const QString &strListingId);
	void refreshPane(KFileTransferPane pane);
	void startCopy(KFileTransferPane sourcePane,
		const QString &strSourceListingId,
		const QStringList &sourceEntryIdList,
		const QString &strDestinationListingId);
	void pauseTask(const QString &strTaskId);
	void resumeTask(const QString &strTaskId);
	void cancelTask(const QString &strTaskId);
	void retryTask(const QString &strTaskId);
	void resolveConflict(const QString &strConflictId,
		KFileTransferConflictResolution resolution,
		bool bApplyToRemaining);
	void clearCompletedTasks();
	void shutdown();

signals:
	void stateChanged(KFileTransferState state,
		bool bAvailable,
		const QString &strStatusCode);
	void paneLoading(KFileTransferPane pane, const QString &strRequestId);
	void paneChanged(const KFileTransferPaneSnapshot &snapshot);
	void snapshotChanged(const QVector<KFileTransferTaskSnapshot> &taskList);
	void taskChanged(const KFileTransferTaskSnapshot &task);
	void taskRemoved(const QString &strTaskId);
	void conflictRequested(const KFileTransferConflictSnapshot &conflict);
	void transferError(const QString &strErrorCode, const QString &strTechnicalMessage);
	void controlledActivityChanged(bool bActive, int nActiveTaskCount);
	void shutdownFinished();

private:
	friend class KFileTransferSessionServicePrivate;
	std::unique_ptr<KFileTransferSessionServicePrivate> m_spPrivate;
};

#endif // _WINREMOTECONTROL_FILE_TRANSFER_FILETRANSFERSESSIONSERVICE_H_
