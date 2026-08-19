#include "adapters/windows/privacy/windowsprivacyoverlayadapter.h"

#include <QtCore/QThread>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <utility>

namespace
{
	constexpr DWORD kExcludeFromCaptureAffinity = 0x00000011;
}

KWindowsPrivacyOverlayAdapter *KWindowsPrivacyOverlayAdapter::s_pActiveHookAdapter = nullptr;

KWindowsPrivacyOverlayAdapter::KWindowsPrivacyOverlayAdapter(QObject *pParent)
	: QObject(pParent)
{
	connect(qApp, &QGuiApplication::screenAdded,
		this, &KWindowsPrivacyOverlayAdapter::handleScreenTopologyChanged);
	connect(qApp, &QGuiApplication::screenRemoved,
		this, &KWindowsPrivacyOverlayAdapter::handleScreenTopologyChanged);
}

KWindowsPrivacyOverlayAdapter::~KWindowsPrivacyOverlayAdapter()
{
	restore();
}

bool KWindowsPrivacyOverlayAdapter::isSupported() const
{
	return QGuiApplication::instance() != nullptr;
}

KPrivacyOperationResult KWindowsPrivacyOverlayAdapter::apply()
{
	Q_ASSERT(QThread::currentThread() == thread());
	if (m_bActive)
		return KPrivacyOperationResult::success();
	destroyWindows();

	const QList<QScreen *> screens = QGuiApplication::screens();
	if (screens.isEmpty())
	{
		return KPrivacyOperationResult::failure(QStringLiteral("overlay_creation_failed"),
			QStringLiteral("No active display is available"));
	}
	for (QScreen *pScreen : screens)
	{
		auto *pWindow = new QWidget(nullptr,
			Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
				| Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
		pWindow->setAttribute(Qt::WA_ShowWithoutActivating);
		pWindow->setAttribute(Qt::WA_TransparentForMouseEvents);
		pWindow->setAutoFillBackground(true);
		QPalette palette = pWindow->palette();
		palette.setColor(QPalette::Window, Qt::black);
		pWindow->setPalette(palette);
		pWindow->setScreen(pScreen);
		pWindow->setGeometry(pScreen->geometry());
		pWindow->show();
		pWindow->raise();
		const HWND hWindow = reinterpret_cast<HWND>(pWindow->winId());
		if (hWindow == nullptr
			|| !SetWindowDisplayAffinity(hWindow, kExcludeFromCaptureAffinity))
		{
			delete pWindow;
			destroyWindows();
			return KPrivacyOperationResult::failure(
				QStringLiteral("capture_exclusion_failed"),
				QStringLiteral("SetWindowDisplayAffinity failed: %1").arg(GetLastError()));
		}
		m_windows.append(pWindow);
	}
	if (!installKeyboardHook())
	{
		destroyWindows();
		return KPrivacyOperationResult::failure(QStringLiteral("overlay_creation_failed"),
			QStringLiteral("Failed to install the local emergency keyboard hook: %1")
				.arg(GetLastError()));
	}
	m_bActive = true;
	return KPrivacyOperationResult::success();
}

KPrivacyOperationResult KWindowsPrivacyOverlayAdapter::restore()
{
	Q_ASSERT(QThread::currentThread() == thread());
	removeKeyboardHook();
	destroyWindows();
	m_bActive = false;
	return KPrivacyOperationResult::success();
}

void KWindowsPrivacyOverlayAdapter::setEmergencyRestoreHandler(
	EmergencyRestoreHandler handler)
{
	m_emergencyRestoreHandler = std::move(handler);
}

bool KWindowsPrivacyOverlayAdapter::isPhysicalRestoreShortcut(
	quint32 nVirtualKey,
	quint32 nFlags,
	bool bControlPressed,
	bool bAltPressed,
	bool bShiftPressed)
{
	const quint32 nInjectedFlags = LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED;
	return nVirtualKey == 'P'
		&& (nFlags & nInjectedFlags) == 0
		&& bControlPressed && bAltPressed && bShiftPressed;
}

void KWindowsPrivacyOverlayAdapter::handleScreenTopologyChanged()
{
	if (!m_bActive)
		return;
	restore();
	const KPrivacyOperationResult result = apply();
	if (!result.bSucceeded)
		invokeEmergencyRestore();
}

LRESULT CALLBACK KWindowsPrivacyOverlayAdapter::keyboardHookProcedure(
	int nCode,
	WPARAM wParam,
	LPARAM lParam)
{
	if (nCode == HC_ACTION
		&& (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
		&& s_pActiveHookAdapter != nullptr)
	{
		const auto *pKeyboard = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
		const bool bControlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool bAltPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
		const bool bShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		if (isPhysicalRestoreShortcut(pKeyboard->vkCode, pKeyboard->flags,
			bControlPressed, bAltPressed, bShiftPressed))
		{
			s_pActiveHookAdapter->invokeEmergencyRestore();
			return 1;
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool KWindowsPrivacyOverlayAdapter::installKeyboardHook()
{
	if (m_hKeyboardHook != nullptr)
		return true;
	s_pActiveHookAdapter = this;
	m_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL,
		&KWindowsPrivacyOverlayAdapter::keyboardHookProcedure,
		GetModuleHandleW(nullptr), 0);
	if (m_hKeyboardHook == nullptr)
		s_pActiveHookAdapter = nullptr;
	return m_hKeyboardHook != nullptr;
}

void KWindowsPrivacyOverlayAdapter::removeKeyboardHook()
{
	if (m_hKeyboardHook != nullptr)
		UnhookWindowsHookEx(m_hKeyboardHook);
	m_hKeyboardHook = nullptr;
	if (s_pActiveHookAdapter == this)
		s_pActiveHookAdapter = nullptr;
}

void KWindowsPrivacyOverlayAdapter::destroyWindows()
{
	for (QWidget *pWindow : std::as_const(m_windows))
	{
		if (pWindow == nullptr)
			continue;
		const HWND hWindow = reinterpret_cast<HWND>(pWindow->winId());
		if (hWindow != nullptr)
			SetWindowDisplayAffinity(hWindow, WDA_NONE);
		delete pWindow;
	}
	m_windows.clear();
}

void KWindowsPrivacyOverlayAdapter::invokeEmergencyRestore()
{
	if (m_emergencyRestoreHandler)
		m_emergencyRestoreHandler();
}
