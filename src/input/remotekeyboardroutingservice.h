#ifndef _WINREMOTECONTROL_INPUT_REMOTEKEYBOARDROUTINGSERVICE_H_
#define _WINREMOTECONTROL_INPUT_REMOTEKEYBOARDROUTINGSERVICE_H_

#include "adapters/windows/input/windowsremotekeyboardcaptureadapter.h"
#include "core/protocol/sessionmessage.h"
#include "core/security/permissionscope.h"
#include "core/session/sessionstatemachine.h"
#include "core/settings/keyboardsettings.h"

#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QVector>

class KKeyboardSettingsService;
class KRemoteKeyboardRoutingServiceTestAccess;

class KRemoteKeyboardRoutingService final : public QObject
{
	Q_OBJECT

public:
	explicit KRemoteKeyboardRoutingService(KKeyboardSettingsService *pSettingsService,
		QObject *pParent = nullptr);
	~KRemoteKeyboardRoutingService() override;

	void setRemoteWindowHandles(HWND hWindow, HWND hVideo);
	bool isCaptureActive() const;

public slots:
	void handleSessionStateChanged(KSessionState state, quint64 nGeneration);
	void handleCapabilitiesChanged(const KNegotiatedCapabilities &capabilities);
	void handlePermissionsChanged(KPermissionScopes permissions);
	void requestCapture();
	void releaseCapture();
	void handleVideoFocusLost();
	void flushPendingModifiers();
	void beginShortcutRecording(const QString &strTarget);
	void cancelShortcutRecording();

signals:
	void remoteKeyRequested(int nVirtualKey,
		int nScanCode,
		bool bPressed,
		bool bExtended,
		bool bAutoRepeat);
	void captureStateChanged(bool bActive, const QString &strMessage);
	void toggleFullscreenRequested();
	void shortcutRecordingStateChanged(bool bRecording,
		const QString &strTarget,
		const QString &strError);

private:
	friend class KRemoteKeyboardRoutingServiceTestAccess;
	enum KMode
	{
		InactiveMode,
		RemoteCaptureMode,
		RecordingMode,
		DrainingMode
	};

	struct KPendingKey
	{
		quint32 nVirtualKey = 0;
		quint32 nScanCode = 0;
		bool bExtended = false;
	};

	bool handleKeyboardEvent(const KWindowsKeyboardEvent &event);
	bool handleRemoteCaptureEvent(const KWindowsKeyboardEvent &event);
	bool handleRecordingEvent(const KWindowsKeyboardEvent &event);
	bool isRemoteCaptureEligible() const;
	bool isRemoteWindowForeground() const;
	KKeyboardShortcut currentShortcut(quint32 nMainVirtualKey) const;
	bool isLocalOnlyShortcut(const KKeyboardShortcut &shortcut) const;
	void consumeShortcut(const KKeyboardShortcut &shortcut, bool bReplayLocally);
	void sendRemoteKey(const KPendingKey &key, bool bPressed, bool bAutoRepeat);
	void releaseSentRemoteKeys();
	void clearPhysicalState();
	void stopHook();
	static quint32 keyId(const KPendingKey &key);

	KKeyboardSettingsService *m_pSettingsService = nullptr;
	KWindowsRemoteKeyboardCaptureAdapter m_captureAdapter;
	QTimer m_recordingTimer;
	KKeyboardSettings m_settings;
	KSessionState m_sessionState = IdleSessionState;
	KNegotiatedCapabilities m_capabilities;
	KPermissionScopes m_permissions;
	QVector<KPendingKey> m_pendingModifiers;
	QSet<quint32> m_physicalModifierKeys;
	QSet<quint32> m_consumedVirtualKeys;
	QSet<quint32> m_sentRemoteKeyIds;
	QHash<quint32, KPendingKey> m_sentRemoteKeys;
	HWND m_hRemoteWindow = nullptr;
	HWND m_hVideoWindow = nullptr;
	quint64 m_nGeneration = 0;
	QString m_strRecordingTarget;
	KMode m_mode = InactiveMode;
};

#endif // _WINREMOTECONTROL_INPUT_REMOTEKEYBOARDROUTINGSERVICE_H_
