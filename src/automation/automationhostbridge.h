#ifndef _WINREMOTECONTROL_AUTOMATIONHOSTBRIDGE_H_
#define _WINREMOTECONTROL_AUTOMATIONHOSTBRIDGE_H_

#include "automation/wrcdriverhostapi.h"
#include "core/privacy/privacytypes.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>

#include <atomic>

class KApplicationCommandRegistry;
class KSessionController;
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

	const KWrcDriverHostApiV1 *hostApi() const;
	void stopAcceptingRequests();

private:
	struct KAutomationEvent
	{
		quint64 nSequence = 0;
		QString strType;
		QJsonObject value;
	};

	static void SubmitCommand(void *pHostContext,
		std::uint64_t nRequestId,
		const char *pCommandIdUtf8,
		std::uint32_t nCommandIdBytes,
		const char *pArgumentsJsonUtf8,
		std::uint32_t nArgumentsJsonBytes,
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
	static void WriteLog(void *pHostContext,
		std::uint32_t nLevel,
		const char *pMessageUtf8,
		std::uint32_t nMessageBytes);

	void submitCommand(quint64 nRequestId,
		const QByteArray &commandIdUtf8,
		const QByteArray &argumentsJsonUtf8,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext);
	void requestSnapshot(quint64 nRequestId,
		const QByteArray &snapshotKindUtf8,
		quint64 nSinceSequence,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext);
	void synchronizeSessionGeneration();
	void initializeStateConnections();
	void appendEvent(const QString &strType, const QJsonObject &value);
	QJsonObject stateSnapshot() const;
	QJsonObject eventsSnapshot(quint64 nSinceSequence) const;
	void completeJson(quint64 nRequestId,
		const QJsonObject &object,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext) const;

	KWrcDriverHostApiV1 m_hostApi;
	KApplicationCommandRegistry *m_pRegistry = nullptr;
	KSessionController *m_pSessionController = nullptr;
	QString m_strDataDirectory;
	QString m_strSessionState = QStringLiteral("Idle");
	QString m_strSignalingState;
	QString m_strWebRtcState;
	QString m_strRemoteDeviceId;
	QString m_strLastError;
	QStringList m_negotiatedCapabilities;
	KPrivacyModeStatus m_privacyModeStatus;
	KPostSessionActionStatus m_postSessionActionStatus;
	QList<KAutomationEvent> m_events;
	quint64 m_nNextEventSequence = 1;
	quint64 m_nReceivedFrameCount = 0;
	qint64 m_nLastFrameTimestampMs = 0;
	int m_nLastFrameWidth = 0;
	int m_nLastFrameHeight = 0;
	quint16 m_nListeningPort = 0;
	std::atomic<quint64> m_nObservedSessionGeneration{0};
	bool m_bAcceptingRequests = true;
	bool m_bListeningAvailable = false;
	bool m_bSessionChannelOpen = false;
	bool m_bInputChannelOpen = false;
	bool m_bClipboardChannelOpen = false;
	bool m_bTerminalChannelOpen = false;
};

#endif // _WINREMOTECONTROL_AUTOMATIONHOSTBRIDGE_H_
