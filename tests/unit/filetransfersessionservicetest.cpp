#include "file_transfer/filetransfersessionservice.h"

#include "core/file_transfer/filesystemport.h"
#include "session/sessioncontroller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QUuid>

#include <iostream>
#include <memory>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	class KFakeFileSystemPort final : public IKFileSystemPort
	{
	public:
		bool listDrives(QVector<KFileListingEntry> *, QString *) override { return false; }
		bool listDirectory(const QString &, QVector<KFileListingEntry> *,
			QString *) override { return false; }
		bool expandDirectoryTree(const QString &, int, KFileTreeExpansion *,
			QString *) override { return false; }
		bool createLocalReference(const QString &, KFileListingEntry *,
			QString *) override { return false; }
		bool destinationExists(const QString &, const QString &, bool *,
			QString *) override { return false; }
		bool prepareRelativeDirectories(const QString &, const QStringList &,
			KDirectoryPreparationResult *, QString *) override { return false; }
		bool cleanupCreatedDirectories(const QStringList &,
			KDirectoryCleanupResult *, QString *) override { return false; }
		bool openSourceSnapshot(const QString &, KFileSourceSnapshot *,
			QString *) override { return false; }
		bool readSourceChunk(const QString &, quint64, int, QByteArray *, bool *,
			QString *) override { return false; }
		bool closeSourceSnapshot(const QString &, QString *) override { return false; }
		bool beginWrite(const KFileWriteRequest &, KFileWriteSession *,
			QString *) override { return false; }
		bool appendWriteChunk(const QString &, const QByteArray &,
			QString *) override { return false; }
		bool finalizeWrite(const QString &, const QByteArray &, KFileWriteResult *,
			QString *) override { return false; }
		bool tryRequestWriteCancellation(const QString &) override { return false; }
		bool abortWrite(const QString &, QString *) override { return false; }
	};

	class KFakeSessionController final : public KSessionController
	{
	public:
		void setRole(const QString &) override {}
		void startSignalingServer(quint16) override {}
		void connectSignaling(const QString &, quint16) override {}
		void retryLastConnection() override {}
		void disconnectSession() override {}
		void enterRemoteDesktop(const KStreamConfig &) override {}
		void leaveRemoteDesktop() override {}
		void startStreaming() override {}
		void stopStreaming() override {}
		void pushVideoFrame(const KVideoFrame &) override {}
		void sendInputMessage(const KInputMessage &) override {}
		void sendClipboardMessage(const KClipboardMessage &) override {}
		bool sendTerminalControlMessage(const KTerminalMessage &) override { return false; }
		bool sendTerminalData(const QByteArray &) override { return false; }
		bool isTerminalBackpressured() const override { return false; }
		bool ensureFileTransferChannels() override
		{
			++nEnsureChannelCount;
			return bEnsureChannelsSucceeds;
		}
		bool sendFileTransferLifecycleMessage(
			const KFileTransferLifecycleMessage &message) override
		{
			lifecycleMessages.append(message);
			return true;
		}
		bool sendFileTransferControlMessage(
			const KFileTransferControlMessage &message) override
		{
			controlMessages.append(message);
			return true;
		}
		void sendStreamConfig(const KStreamConfig &) override {}
		QString requestPrivacyMode(KPrivacyMode) override { return QString(); }
		QString requestPostSessionAction(KPostSessionAction) override { return QString(); }
		void handleCaptureFailure() override {}
		void applyApplicationSettings(const KApplicationSettings &) override {}
		void respondIncomingAccessRequest(const QString &, bool) override {}
		void respondPairingRequest(const QString &, bool,
			KPermissionScopes) override {}
		quint64 sessionGeneration() const override { return nGeneration; }
		KSessionRole sessionRole() const override { return role; }
		bool isIdle() const override { return false; }
		bool matchesCurrentEndpoint(const QString &, quint16) const override { return true; }

		void makeAvailable()
		{
			emit sessionStateChanged(ConnectedSessionState);
			KNegotiatedCapabilities capabilities;
			capabilities.bValid = true;
			capabilities.bFileTransfer = true;
			capabilities.channels = {
				QStringLiteral("file-control"), QStringLiteral("file-data")
			};
			emit sessionCapabilitiesChanged(capabilities);
		}

		quint64 nGeneration = 1;
		KSessionRole role = ControllerSessionRole;
		int nEnsureChannelCount = 0;
		bool bEnsureChannelsSucceeds = true;
		QVector<KFileTransferLifecycleMessage> lifecycleMessages;
		QVector<KFileTransferControlMessage> controlMessages;
	};

	std::unique_ptr<IKFileSystemPort> CreateFileSystem()
	{
		return std::make_unique<KFakeFileSystemPort>();
	}

	KFileTransferLifecycleMessage NewOpenRequest(quint64 nPeerGeneration)
	{
		KFileTransferLifecycleMessage message;
		message.type = OpenRequestFileTransferLifecycleMessageType;
		message.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		message.nGeneration = nPeerGeneration;
		return message;
	}

	void TestDifferentPeerGenerationsCompleteHandshake()
	{
		KFakeSessionController controller;
		controller.role = ControllerSessionRole;
		controller.nGeneration = 3;
		KFileTransferSessionService controllerService(CreateFileSystem(), &controller);
		controller.makeAvailable();

		KFakeSessionController controlled;
		controlled.role = ControlledSessionRole;
		controlled.nGeneration = 6;
		KFileTransferSessionService controlledService(CreateFileSystem(), &controlled);
		controlled.makeAvailable();

		controllerService.openCurrentFileTransfer();
		Check(controller.lifecycleMessages.size() == 1,
			QStringLiteral("controller sends one open request"));
		const KFileTransferLifecycleMessage request = controller.lifecycleMessages.first();
		Check(request.nGeneration == 3,
			QStringLiteral("open request retains controller diagnostic generation"));

		emit controlled.fileTransferLifecycleMessageReceived(request);
		Check(controlled.lifecycleMessages.size() == 1,
			QStringLiteral("controlled side accepts a request from a different generation"));
		const KFileTransferLifecycleMessage accepted = controlled.lifecycleMessages.first();
		Check(accepted.type == OpenAcceptedFileTransferLifecycleMessageType,
			QStringLiteral("controlled side sends open accepted"));
		Check(accepted.nGeneration == 6,
			QStringLiteral("response carries controlled diagnostic generation"));

		emit controller.fileTransferLifecycleMessageReceived(accepted);
		Check(controller.nEnsureChannelCount == 1,
			QStringLiteral("controller creates channels after cross-generation acceptance"));
		emit controller.fileTransferChannelsChanged(true, true);
		emit controlled.fileTransferChannelsChanged(true, true);
		Check(controllerService.state() == ReadyFileTransferState,
			QStringLiteral("controller reaches Ready after channels open"));
		Check(controlledService.state() == ReadyFileTransferState,
			QStringLiteral("controlled side reaches Ready after channels open"));
	}

	void TestSameGenerationStillCompletesHandshake()
	{
		KFakeSessionController controlled;
		controlled.role = ControlledSessionRole;
		controlled.nGeneration = 9;
		KFileTransferSessionService service(CreateFileSystem(), &controlled);
		controlled.makeAvailable();

		emit controlled.fileTransferLifecycleMessageReceived(NewOpenRequest(9));
		Check(controlled.lifecycleMessages.size() == 1
			&& controlled.lifecycleMessages.first().type
				== OpenAcceptedFileTransferLifecycleMessageType,
			QStringLiteral("same-generation request remains accepted"));
	}

	void TestMismatchedRequestIdIsIgnored()
	{
		KFakeSessionController controller;
		controller.role = ControllerSessionRole;
		controller.nGeneration = 3;
		KFileTransferSessionService service(CreateFileSystem(), &controller);
		controller.makeAvailable();
		service.openCurrentFileTransfer();

		KFileTransferLifecycleMessage accepted;
		accepted.type = OpenAcceptedFileTransferLifecycleMessageType;
		accepted.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		accepted.nGeneration = 99;
		emit controller.fileTransferLifecycleMessageReceived(accepted);
		Check(controller.nEnsureChannelCount == 0,
			QStringLiteral("mismatched request id cannot create channels"));
	}

	void TestChangedLocalGenerationRejectsLateResponse()
	{
		KFakeSessionController controller;
		controller.role = ControllerSessionRole;
		controller.nGeneration = 3;
		KFileTransferSessionService service(CreateFileSystem(), &controller);
		controller.makeAvailable();
		service.openCurrentFileTransfer();

		KFileTransferLifecycleMessage accepted = controller.lifecycleMessages.first();
		accepted.type = OpenAcceptedFileTransferLifecycleMessageType;
		accepted.nGeneration = 6;
		controller.nGeneration = 4;
		emit controller.fileTransferLifecycleMessageReceived(accepted);
		Check(controller.nEnsureChannelCount == 0,
			QStringLiteral("late response cannot cross the local session generation"));
	}

	void TestUnavailableControlledSideRejectsImmediately()
	{
		KFakeSessionController controlled;
		controlled.role = ControlledSessionRole;
		controlled.nGeneration = 6;
		KFileTransferSessionService service(CreateFileSystem(), &controlled);
		emit controlled.sessionStateChanged(ConnectedSessionState);

		emit controlled.fileTransferLifecycleMessageReceived(NewOpenRequest(3));
		Check(controlled.lifecycleMessages.size() == 1,
			QStringLiteral("unavailable controlled side responds without a timeout"));
		Check(controlled.lifecycleMessages.first().type
				== OpenRejectedFileTransferLifecycleMessageType
			&& controlled.lifecycleMessages.first().strErrorCode
				== QStringLiteral("permission_denied"),
			QStringLiteral("unavailable controlled side returns permission_denied"));
	}

	void TestSnapshotPublishesStateBeforePaneData()
	{
		KFakeSessionController controller;
		KFileTransferSessionService service(CreateFileSystem(), &controller);
		controller.makeAvailable();

		QStringList eventList;
		QObject::connect(&service, &KFileTransferSessionService::stateChanged,
			&service, [&eventList](KFileTransferState, bool, const QString &)
			{
				eventList.append(QStringLiteral("state"));
			});
		QObject::connect(&service, &KFileTransferSessionService::snapshotChanged,
			&service, [&eventList](const QVector<KFileTransferTaskSnapshot> &)
			{
				eventList.append(QStringLiteral("tasks"));
			});
		QObject::connect(&service, &KFileTransferSessionService::paneChanged,
			&service, [&eventList](const KFileTransferPaneSnapshot &)
			{
				eventList.append(QStringLiteral("pane"));
			});

		service.requestSnapshot();
		Check(eventList.size() == 4
			&& eventList.at(0) == QStringLiteral("state")
			&& eventList.at(1) == QStringLiteral("tasks")
			&& eventList.at(2) == QStringLiteral("pane")
			&& eventList.at(3) == QStringLiteral("pane"),
			QStringLiteral("snapshot publishes generation-bearing state before pane data"));
	}

	void TestReadyInitializationIsIdempotent()
	{
		KFakeSessionController controller;
		KFileTransferSessionService service(CreateFileSystem(), &controller);
		controller.makeAvailable();
		int nReadyCount = 0;
		QObject::connect(&service, &KFileTransferSessionService::stateChanged,
			&service, [&nReadyCount](KFileTransferState state, bool, const QString &)
			{
				if (state == ReadyFileTransferState)
					++nReadyCount;
			});

		service.openCurrentFileTransfer();
		KFileTransferLifecycleMessage accepted = controller.lifecycleMessages.first();
		accepted.type = OpenAcceptedFileTransferLifecycleMessageType;
		emit controller.fileTransferChannelsChanged(true, false);
		emit controller.fileTransferLifecycleMessageReceived(accepted);
		emit controller.fileTransferChannelsChanged(true, true);
		emit controller.fileTransferChannelsChanged(true, true);

		int nRootRequestCount = 0;
		for (const KFileTransferControlMessage &message : controller.controlMessages)
		{
			if (message.type == ListRootsRequestFileTransferControlMessageType)
				++nRootRequestCount;
		}
		Check(nReadyCount == 1,
			QStringLiteral("repeated channel state enters Ready only once"));
		Check(nRootRequestCount == 1,
			QStringLiteral("Ready initialization requests remote roots only once"));
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestDifferentPeerGenerationsCompleteHandshake();
	TestSameGenerationStillCompletesHandshake();
	TestMismatchedRequestIdIsIgnored();
	TestChangedLocalGenerationRejectsLateResponse();
	TestUnavailableControlledSideRejectsImmediately();
	TestSnapshotPublishesStateBeforePaneData();
	TestReadyInitializationIsIdempotent();
	return g_nFailureCount == 0 ? 0 : 1;
}
