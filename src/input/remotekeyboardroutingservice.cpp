#include "input/remotekeyboardroutingservice.h"

#include "settings/keyboardsettingsservice.h"

#include <QtCore/QMetaObject>

namespace
{
	constexpr int kShortcutRecordingTimeoutMs = 10000;

	bool IsExtendedKey(const KWindowsKeyboardEvent &event)
	{
		return (event.nFlags & LLKHF_EXTENDED) != 0;
	}
}

KRemoteKeyboardRoutingService::KRemoteKeyboardRoutingService(
	KKeyboardSettingsService *pSettingsService,
	QObject *pParent)
	: QObject(pParent)
	, m_pSettingsService(pSettingsService)
	, m_captureAdapter(this)
{
	Q_ASSERT(m_pSettingsService != nullptr);
	m_settings = m_pSettingsService->settings();
	connect(m_pSettingsService, &KKeyboardSettingsService::settingsChanged,
		this, [this](const KKeyboardSettings &settings)
		{ m_settings = settings; });
	m_recordingTimer.setSingleShot(true);
	connect(&m_recordingTimer, &QTimer::timeout,
		this, [this]()
		{
			const QString strTarget = m_strRecordingTarget;
			stopHook();
			emit shortcutRecordingStateChanged(false, strTarget,
				QStringLiteral("快捷键录制已超时"));
		});
}

KRemoteKeyboardRoutingService::~KRemoteKeyboardRoutingService()
{
	releaseSentRemoteKeys();
	stopHook();
}

void KRemoteKeyboardRoutingService::setRemoteWindowHandles(HWND hWindow, HWND hVideo)
{
	m_hRemoteWindow = hWindow;
	m_hVideoWindow = hVideo;
}

bool KRemoteKeyboardRoutingService::isCaptureActive() const
{
	return m_mode == RemoteCaptureMode;
}

void KRemoteKeyboardRoutingService::handleSessionStateChanged(
	KSessionState state,
	quint64 nGeneration)
{
	const bool bGenerationChanged = m_nGeneration != 0 && m_nGeneration != nGeneration;
	m_sessionState = state;
	m_nGeneration = nGeneration;
	if (bGenerationChanged || state != StreamingSessionState)
		handleVideoFocusLost();
}

void KRemoteKeyboardRoutingService::handleCapabilitiesChanged(
	const KNegotiatedCapabilities &capabilities)
{
	m_capabilities = capabilities;
	if (!isRemoteCaptureEligible())
		handleVideoFocusLost();
}

void KRemoteKeyboardRoutingService::handlePermissionsChanged(KPermissionScopes permissions)
{
	m_permissions = permissions;
	if (!isRemoteCaptureEligible())
		handleVideoFocusLost();
}

void KRemoteKeyboardRoutingService::requestCapture()
{
	if (!isRemoteCaptureEligible() || !isRemoteWindowForeground())
		return;
	if (m_mode == RemoteCaptureMode)
		return;
	stopHook();
	QString strError;
	if (!m_captureAdapter.start(
		[this](const KWindowsKeyboardEvent &event)
		{ return handleKeyboardEvent(event); }, &strError))
	{
		emit captureStateChanged(false, strError);
		return;
	}
	m_mode = RemoteCaptureMode;
	emit captureStateChanged(true,
		QStringLiteral("键盘已发送到远端，按 Ctrl + Alt + Enter 释放"));
}

void KRemoteKeyboardRoutingService::releaseCapture()
{
	if (m_mode != RemoteCaptureMode && m_mode != DrainingMode)
		return;
	releaseSentRemoteKeys();
	m_pendingModifiers.clear();
	if (m_physicalModifierKeys.isEmpty() && m_consumedVirtualKeys.isEmpty())
		stopHook();
	else
		m_mode = DrainingMode;
	emit captureStateChanged(false, QStringLiteral("单击远程画面重新捕获键盘"));
}

void KRemoteKeyboardRoutingService::handleVideoFocusLost()
{
	if (m_mode != RemoteCaptureMode && m_mode != DrainingMode)
		return;
	releaseSentRemoteKeys();
	stopHook();
	emit captureStateChanged(false, QStringLiteral("单击远程画面重新捕获键盘"));
}

void KRemoteKeyboardRoutingService::flushPendingModifiers()
{
	if (m_mode != RemoteCaptureMode)
		return;
	for (const KPendingKey &key : std::as_const(m_pendingModifiers))
		sendRemoteKey(key, true, false);
	m_pendingModifiers.clear();
}

void KRemoteKeyboardRoutingService::beginShortcutRecording(const QString &strTarget)
{
	if (strTarget != QStringLiteral("localOnly")
		&& strTarget != QStringLiteral("toggleFullscreen"))
	{
		emit shortcutRecordingStateChanged(false, strTarget,
			QStringLiteral("快捷键录制目标无效"));
		return;
	}
	handleVideoFocusLost();
	stopHook();
	m_strRecordingTarget = strTarget;
	QString strError;
	if (!m_captureAdapter.start(
		[this](const KWindowsKeyboardEvent &event)
		{ return handleKeyboardEvent(event); }, &strError))
	{
		emit shortcutRecordingStateChanged(false, strTarget, strError);
		return;
	}
	m_mode = RecordingMode;
	m_recordingTimer.start(kShortcutRecordingTimeoutMs);
	emit shortcutRecordingStateChanged(true, strTarget, QString());
}

void KRemoteKeyboardRoutingService::cancelShortcutRecording()
{
	if (m_mode != RecordingMode)
		return;
	const QString strTarget = m_strRecordingTarget;
	stopHook();
	emit shortcutRecordingStateChanged(false, strTarget, QString());
}

bool KRemoteKeyboardRoutingService::handleKeyboardEvent(
	const KWindowsKeyboardEvent &event)
{
	if (m_mode == RecordingMode)
		return handleRecordingEvent(event);
	if (m_mode == DrainingMode)
	{
		if (!event.bPressed)
		{
			m_physicalModifierKeys.remove(event.nVirtualKey);
			m_consumedVirtualKeys.remove(event.nVirtualKey);
			if (m_physicalModifierKeys.isEmpty() && m_consumedVirtualKeys.isEmpty())
				QMetaObject::invokeMethod(this, [this]() { stopHook(); }, Qt::QueuedConnection);
		}
		return true;
	}
	if (m_mode != RemoteCaptureMode)
		return false;
	if (!isRemoteCaptureEligible() || !isRemoteWindowForeground())
	{
		QMetaObject::invokeMethod(this, &KRemoteKeyboardRoutingService::handleVideoFocusLost,
			Qt::QueuedConnection);
		return false;
	}
	return handleRemoteCaptureEvent(event);
}

bool KRemoteKeyboardRoutingService::handleRemoteCaptureEvent(
	const KWindowsKeyboardEvent &event)
{
	const KKeyboardShortcutModifier modifier = ModifierForVirtualKey(event.nVirtualKey);
	if (modifier != NoKeyboardShortcutModifier)
	{
		if (event.bPressed)
		{
			if (!m_physicalModifierKeys.contains(event.nVirtualKey))
			{
				m_physicalModifierKeys.insert(event.nVirtualKey);
				m_pendingModifiers.append({ event.nVirtualKey, event.nScanCode,
					IsExtendedKey(event) });
			}
			return true;
		}
		m_physicalModifierKeys.remove(event.nVirtualKey);
		if (m_consumedVirtualKeys.remove(event.nVirtualKey))
			return true;
		flushPendingModifiers();
		KPendingKey key { event.nVirtualKey, event.nScanCode, IsExtendedKey(event) };
		if (m_sentRemoteKeyIds.contains(keyId(key)))
			sendRemoteKey(key, false, false);
		return true;
	}

	const KPendingKey key { event.nVirtualKey, event.nScanCode, IsExtendedKey(event) };
	if (!event.bPressed)
	{
		if (m_consumedVirtualKeys.remove(event.nVirtualKey))
			return true;
		if (m_sentRemoteKeyIds.contains(keyId(key)))
			sendRemoteKey(key, false, false);
		return true;
	}

	const KKeyboardShortcut shortcut = currentShortcut(event.nVirtualKey);
	if (shortcut == DefaultReleaseKeyboardCaptureShortcut())
	{
		consumeShortcut(shortcut, false);
		QMetaObject::invokeMethod(this, &KRemoteKeyboardRoutingService::releaseCapture,
			Qt::QueuedConnection);
		return true;
	}
	if (shortcut == m_settings.toggleFullscreenShortcut)
	{
		consumeShortcut(shortcut, false);
		QMetaObject::invokeMethod(this, [this]() { emit toggleFullscreenRequested(); },
			Qt::QueuedConnection);
		return true;
	}
	if (isLocalOnlyShortcut(shortcut))
	{
		consumeShortcut(shortcut, true);
		return true;
	}

	flushPendingModifiers();
	const bool bAutoRepeat = m_sentRemoteKeyIds.contains(keyId(key));
	sendRemoteKey(key, true, bAutoRepeat);
	return true;
}

bool KRemoteKeyboardRoutingService::handleRecordingEvent(
	const KWindowsKeyboardEvent &event)
{
	const KKeyboardShortcutModifier modifier = ModifierForVirtualKey(event.nVirtualKey);
	if (modifier != NoKeyboardShortcutModifier)
	{
		if (event.bPressed)
			m_physicalModifierKeys.insert(event.nVirtualKey);
		else
			m_physicalModifierKeys.remove(event.nVirtualKey);
		return true;
	}
	if (!event.bPressed)
		return true;
	if (event.nVirtualKey == VK_ESCAPE)
	{
		QMetaObject::invokeMethod(this, &KRemoteKeyboardRoutingService::cancelShortcutRecording,
			Qt::QueuedConnection);
		return true;
	}
	const KKeyboardShortcut shortcut = currentShortcut(event.nVirtualKey);
	const QString strTarget = m_strRecordingTarget;
	QMetaObject::invokeMethod(this, [this, shortcut, strTarget]()
	{
		stopHook();
		if (!shortcut.isValid()
			|| shortcut.modifiers == NoKeyboardShortcutModifier
			|| IsSecureAttentionShortcut(shortcut))
		{
			emit shortcutRecordingStateChanged(false, strTarget,
				QStringLiteral("快捷键必须包含修饰键，且不能是 Ctrl+Alt+Delete"));
			return;
		}
		const QString strShortcut = KeyboardShortcutCanonicalName(shortcut);
		if (strTarget == QStringLiteral("localOnly"))
			m_pSettingsService->addLocalShortcut(strShortcut);
		else
			m_pSettingsService->updateToggleFullscreenShortcut(strShortcut);
		emit shortcutRecordingStateChanged(false, strTarget, QString());
	}, Qt::QueuedConnection);
	return true;
}

bool KRemoteKeyboardRoutingService::isRemoteCaptureEligible() const
{
	return m_sessionState == StreamingSessionState
		&& m_capabilities.bValid
		&& m_capabilities.bKeyboard
		&& m_permissions.testFlag(InputControlPermissionScope)
		&& m_hRemoteWindow != nullptr
		&& m_hVideoWindow != nullptr;
}

bool KRemoteKeyboardRoutingService::isRemoteWindowForeground() const
{
	return m_hRemoteWindow != nullptr && GetForegroundWindow() == m_hRemoteWindow;
}

KKeyboardShortcut KRemoteKeyboardRoutingService::currentShortcut(
	quint32 nMainVirtualKey) const
{
	KKeyboardShortcut shortcut;
	shortcut.nVirtualKey = NormalizeShortcutVirtualKey(nMainVirtualKey);
	for (quint32 nVirtualKey : m_physicalModifierKeys)
		shortcut.modifiers |= ModifierForVirtualKey(nVirtualKey);
	return shortcut;
}

bool KRemoteKeyboardRoutingService::isLocalOnlyShortcut(
	const KKeyboardShortcut &shortcut) const
{
	return m_settings.localOnlyShortcuts.contains(shortcut);
}

void KRemoteKeyboardRoutingService::consumeShortcut(
	const KKeyboardShortcut &shortcut,
	bool bReplayLocally)
{
	for (quint32 nVirtualKey : m_physicalModifierKeys)
		m_consumedVirtualKeys.insert(nVirtualKey);
	m_consumedVirtualKeys.insert(shortcut.nVirtualKey);
	m_pendingModifiers.clear();
	if (bReplayLocally)
	{
		QMetaObject::invokeMethod(this, [this, shortcut]()
		{ m_captureAdapter.replayLocalShortcut(shortcut); }, Qt::QueuedConnection);
	}
}

void KRemoteKeyboardRoutingService::sendRemoteKey(
	const KPendingKey &key,
	bool bPressed,
	bool bAutoRepeat)
{
	const quint32 nId = keyId(key);
	if (bPressed)
	{
		m_sentRemoteKeyIds.insert(nId);
		m_sentRemoteKeys.insert(nId, key);
	}
	else
	{
		m_sentRemoteKeyIds.remove(nId);
		m_sentRemoteKeys.remove(nId);
	}
	emit remoteKeyRequested(static_cast<int>(key.nVirtualKey),
		static_cast<int>(key.nScanCode), bPressed, key.bExtended, bAutoRepeat);
}

void KRemoteKeyboardRoutingService::releaseSentRemoteKeys()
{
	const QList<KPendingKey> keys = m_sentRemoteKeys.values();
	m_sentRemoteKeys.clear();
	m_sentRemoteKeyIds.clear();
	for (const KPendingKey &key : keys)
	{
		emit remoteKeyRequested(static_cast<int>(key.nVirtualKey),
			static_cast<int>(key.nScanCode), false, key.bExtended, false);
	}
}

void KRemoteKeyboardRoutingService::clearPhysicalState()
{
	m_pendingModifiers.clear();
	m_physicalModifierKeys.clear();
	m_consumedVirtualKeys.clear();
}

void KRemoteKeyboardRoutingService::stopHook()
{
	m_recordingTimer.stop();
	m_captureAdapter.stop();
	m_mode = InactiveMode;
	m_strRecordingTarget.clear();
	clearPhysicalState();
}

quint32 KRemoteKeyboardRoutingService::keyId(const KPendingKey &key)
{
	return key.nVirtualKey | (key.nScanCode << 8) | (key.bExtended ? 0x20000 : 0);
}
