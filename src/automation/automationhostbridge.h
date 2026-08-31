#ifndef _WINREMOTECONTROL_AUTOMATIONHOSTBRIDGE_H_
#define _WINREMOTECONTROL_AUTOMATIONHOSTBRIDGE_H_

#include "automation/wrcdriverhostapi.h"
#include "core/privacy/privacytypes.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QSet>

#include <atomic>

class KApplicationCommandRegistry;
class KSessionController;
class KSessionCoordinator;
class KDeviceDiscoveryViewModel;
class KRecentDeviceService;
class KApplicationSettingsService;
class KClipboardSyncService;
class KTerminalSessionService;
class KFileTransferSessionService;
struct KNegotiatedCapabilities;
struct KSessionError;

class KAutomationHostBridge : public QObject
{
	Q_OBJECT

public:
	explicit KAutomationHostBridge(KApplicationCommandRegistry *pRegistry,
		KSessionController *pSessionController,
		const QString &strDataDirectory,
		QObject *pParent = nullptr);
	~KAutomationHostBridge() override;

	KAutomationHostBridge(const KAutomationHostBridge &) = delete;
	KAutomationHostBridge &operator=(const KAutomationHostBridge &) = delete;

	const KWrcDriverHostApiV2 *hostApi() const;
	void observeApplicationFeatures(KSessionCoordinator *pSessionCoordinator,
		KDeviceDiscoveryViewModel *pDiscoveryViewModel,
		KRecentDeviceService *pRecentDeviceService,
		KApplicationSettingsService *pSettingsService,
		KClipboardSyncService *pClipboardService,
		KTerminalSessionService *pTerminalService,
		KFileTransferSessionService *pFileTransferService);
	void setHostReady();
	void stopAcceptingRequests();

private:
	enum KAutomationEventCategory
	{
		CriticalAutomationEventCategory,
		StateAutomationEventCategory,
		TelemetryAutomationEventCategory
	};

	struct KAutomationEvent
	{
		quint64 nSequence = 0;
		KAutomationEventCategory category = StateAutomationEventCategory;
		QString strType;
		QJsonObject value;
	};

	static void SubmitCommand(void *pHostContext,
		std::uint64_t nRequestId,
		const char *pCommandIdUtf8,
		std::uint32_t nCommandIdBytes,
		const char *pArgumentsJsonUtf8,
		std::uint32_t nArgumentsJsonBytes,
		std::uint32_t nTimeoutMs,
		KWrcDriverCommandStartedCallback pStartedCallback,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext);
	static void RequestSnapshot(void *pHostContext,
		std::uint64_t nRequestId,
		const char *pSnapshotKindUtf8,
		std::uint32_t nSnapshotKindBytes,
		std::uint64_t nSinceSequence,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext);
	static std::uint32_t CopyHostValue(void *pHostContext,
		const char *pKeyUtf8,
		std::uint32_t nKeyBytes,
		char *pDestinationUtf8,
		std::uint32_t nDestinationBytes);
	static bool IsHostReady(void *pHostContext);
	static void WriteLog(void *pHostContext,
		std::uint32_t nLevel,
		const char *pMessageUtf8,
		std::uint32_t nMessageBytes);

	void submitCommand(quint64 nRequestId,
		const QByteArray &commandIdUtf8,
		const QByteArray &argumentsJsonUtf8,
		quint32 nTimeoutMs,
		KWrcDriverCommandStartedCallback pStartedCallback,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext);
	void requestSnapshot(quint64 nRequestId,
		const QByteArray &snapshotKindUtf8,
		quint64 nSinceSequence,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext);
	void synchronizeSessionGeneration();
	void initializeStateConnections();
	void appendEvent(KAutomationEventCategory category,
		const QString &strType,
		const QJsonObject &value);
	QJsonObject sessionErrorObject(const KSessionError &error) const;
	QJsonObject stateSnapshot() const;
	QJsonObject eventsSnapshot(quint64 nSinceSequence) const;
	void completeJson(quint64 nRequestId,
		const QJsonObject &object,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext) const;

	KWrcDriverHostApiV2 m_hostApi;
	KApplicationCommandRegistry *m_pRegistry = nullptr;
	KSessionController *m_pSessionController = nullptr;
	QString m_strDataDirectory;
	QString m_strSessionState = QStringLiteral("Idle");
	QString m_strSignalingState;
	QString m_strWebRtcState;
	QString m_strRemoteDeviceId;
	QJsonObject m_currentError;
	QJsonObject m_lastError;
	QStringList m_negotiatedCapabilities;
	QJsonArray m_lanDevices;
	QJsonArray m_recentDevices;
	QJsonArray m_trustedDevices;
	QJsonObject m_applicationSettings;
	QJsonObject m_clipboardState;
	QJsonObject m_terminalState;
	QJsonObject m_fileTransferState;
	QJsonObject m_localFilePane;
	QJsonObject m_remoteFilePane;
	QJsonArray m_fileTransferTasks;
	QJsonObject m_fileTransferConflict;
	KPrivacyModeStatus m_privacyModeStatus;
	KPostSessionActionStatus m_postSessionActionStatus;
	QList<KAutomationEvent> m_events;
	QList<quint64> m_commandRequestOrder;
	QSet<quint64> m_seenCommandRequestIds;
	quint64 m_nNextEventSequence = 1;
	QElapsedTimer m_frameProgressEventTimer;
	quint64 m_nReceivedFrameCount = 0;
	qint64 m_nLastFrameTimestampMs = 0;
	int m_nLastFrameWidth = 0;
	int m_nLastFrameHeight = 0;
	quint16 m_nListeningPort = 0;
	std::atomic<quint64> m_nObservedSessionGeneration{0};
	std::atomic<quint64> m_nPublishedEventCursor{0};
	std::atomic_bool m_bHostReady{false};
	bool m_bAcceptingRequests = true;
	bool m_bListeningAvailable = false;
	bool m_bSessionChannelOpen = false;
	bool m_bInputChannelOpen = false;
	bool m_bClipboardChannelOpen = false;
	bool m_bTerminalChannelOpen = false;
};

#endif // _WINREMOTECONTROL_AUTOMATIONHOSTBRIDGE_H_
