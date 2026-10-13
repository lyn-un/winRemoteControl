#include "render/videorenderwidget.h"
#include "common/sessiontracelogger.h"

#include "common/latencytracelogger.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QMetaObject>
#include <QtCore/QString>
#include <QtGui/QFocusEvent>
#include <QtGui/QHideEvent>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEngine>
#include <QtGui/QWheelEvent>

#include <d3dcompiler.h>

#include <cstring>
#include <iterator>
#include <utility>
#include <chrono>

namespace
{
	constexpr float kClearColor[4] = { 0.97f, 0.98f, 0.99f, 1.0f };
	constexpr int kRemoteMouseButtonLeft = 1;
	constexpr int kRemoteMouseButtonRight = 2;
	constexpr int kRemoteMouseButtonMiddle = 3;
	constexpr int kRemoteMouseButtonX1 = 4;
	constexpr int kRemoteMouseButtonX2 = 5;
	constexpr qint64 kMouseMoveThrottleMs = 16;
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr quint32 kScanCodeShift = 8;
	constexpr quint32 kExtendedKeyMask = 0x20000;

	const char *kVertexShaderSource =
		"struct VSInput { float2 position : POSITION; float2 texcoord : TEXCOORD0; };"
		"struct PSInput { float4 position : SV_POSITION; float2 texcoord : TEXCOORD0; };"
		"PSInput main(VSInput input) {"
		"    PSInput output;"
		"    output.position = float4(input.position, 0.0f, 1.0f);"
		"    output.texcoord = input.texcoord;"
		"    return output;"
		"}";

	const char *kPixelShaderSource =
		"Texture2D frameTexture : register(t0);"
		"SamplerState frameSampler : register(s0);"
		"float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {"
		"    return frameTexture.Sample(frameSampler, texcoord);"
		"}";

	qint64 SteadyNowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}

KVideoRenderWidget::KVideoRenderWidget(QWidget *pParent)
	: QWidget(pParent)
{
	setAttribute(Qt::WA_NativeWindow, true);
	setAttribute(Qt::WA_DontCreateNativeAncestors, true);
	setAttribute(Qt::WA_PaintOnScreen, true);
	setAutoFillBackground(false);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	m_mouseMoveFlushTimer.setSingleShot(true);
	m_mouseMoveFlushTimer.setTimerType(Qt::PreciseTimer);
	connect(&m_mouseMoveFlushTimer, &QTimer::timeout,
		this, &KVideoRenderWidget::flushPendingMouseMove);
	m_mouseMoveThrottleTimer.start();
}

KVideoRenderWidget::~KVideoRenderWidget()
{
	setAttribute(Qt::WA_InputMethodEnabled, true);
	cancelPendingMouseMove();
	releaseAll();
}

void KVideoRenderWidget::enqueueFrame(const KDecodedVideoFrame &frame)
{
	if (!frame.hasPixels() || frame.nWidth <= 0 || frame.nHeight <= 0)
		return;

	const KLatestDecodedFrameEnqueueResult enqueueResult =
		m_frameQueue.enqueue(frame, SteadyNowMs());

	if (enqueueResult.bFrameCoalesced
		&& KLatencyTraceLogger::isEnabled()
		&& (enqueueResult.stats.nCoalescedFrames == 1
			|| enqueueResult.stats.nCoalescedFrames
				% kVideoTraceFrameInterval == 0))
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("render_frame_coalesced"),
			QStringLiteral("stage=gui_queue receivedTotal=%1 coalescedTotal=%2 latestFrame=%3")
				.arg(enqueueResult.stats.nReceivedFrames)
				.arg(enqueueResult.stats.nCoalescedFrames)
				.arg(frame.nFrameIndex));
	}

	if (enqueueResult.bNeedsPresentation)
		QMetaObject::invokeMethod(this, &KVideoRenderWidget::presentLatestFrame, Qt::QueuedConnection);
}

void KVideoRenderWidget::presentLatestFrame()
{
	KDecodedVideoFrame frame;
	if (!m_frameQueue.takeLatest(&frame))
		return;

	presentFrame(frame);

	if (m_frameQueue.completePresentation())
		QMetaObject::invokeMethod(this, &KVideoRenderWidget::presentLatestFrame, Qt::QueuedConnection);
}

void KVideoRenderWidget::presentFrame(const KDecodedVideoFrame &frame)
{
	if (!frame.hasPixels() || frame.nWidth <= 0 || frame.nHeight <= 0)
		return;

	const qint64 nPresentBeginAtMs = SteadyNowMs();
	const bool bTraceFrame = frame.nFrameIndex > 0 && frame.nFrameIndex % kVideoTraceFrameInterval == 0;
	QElapsedTimer renderTimer;
	if (bTraceFrame)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("render_begin"),
			QStringLiteral("frame=%1 sourceFrame=%2 width=%3 height=%4 timestampMs=%5 lowLatencyRender=%6 lastInputSeq=%7 inputAgeMs=%8")
				.arg(frame.nFrameIndex)
				.arg(frame.nSourceFrameIndex)
				.arg(frame.nWidth)
				.arg(frame.nHeight)
				.arg(frame.nTimestampMs)
				.arg(frame.bWebRtcLowLatencyRender ? 1 : 0)
				.arg(frame.nLastInputSeq)
				.arg(frame.nInputAgeMs));
		renderTimer.start();
	}

	QString strError;
	if (!initializeD3d(&strError) ||
		!updateVertexBuffer(frame.nWidth, frame.nHeight, &strError))
	{
		emit renderError(strError);
		return;
	}

	QElapsedTimer uploadTimer;
	uploadTimer.start();
	const bool bI420Frame = frame.pixelFormat == KDecodedPixelFormat::I420;
	quint64 nUploadBytes = static_cast<quint64>(frame.nWidth) * frame.nHeight * 4;
	if (bI420Frame)
	{
		if (frame.spRenderSession && frame.spRenderSession->bForceBgra.load())
			return;
		const HRESULT hr = m_i420Pipeline.upload(m_spDevice.Get(), m_spContext.Get(), frame);
		if (FAILED(hr))
		{
			m_bI420Frame = false;
			m_i420Pipeline.clear();
			m_spFrameShaderResourceView.Reset();
			m_spFrameTexture.Reset();
			const HRESULT hrDevice = m_spDevice->GetDeviceRemovedReason();
			if (FAILED(hrDevice))
			{
				emit renderError(hresultMessage(QStringLiteral("D3D11 device failed"), hrDevice));
				return;
			}
			if (frame.spRenderSession && frame.spRenderSession->bActive.load())
				frame.spRenderSession->bForceBgra.store(true);
			KSessionTraceLogger::write(QStringLiteral("controller"), QStringLiteral("video_renderer_fallback"),
				QStringLiteral("bgra"), -1, QStringLiteral("reason=i420_resource_failure generation=%1 hresult=%2")
					.arg(frame.spRenderSession ? frame.spRenderSession->nGeneration : 0).arg(static_cast<quint32>(hr)));
			return;
		}
		m_nFrameWidth = frame.nWidth;
		m_nFrameHeight = frame.nHeight;
		nUploadBytes = 0;
		for (const KVideoPlane &plane : frame.planes)
			nUploadBytes += static_cast<quint64>(plane.nWidth) * plane.nHeight;
	}
	else
	{
		// Capture a valid pointer before submission. A concurrent session retirement
		// may make pixelData() return null, but cannot free this frame's owned pixels.
		const unsigned char *pPixels = frame.pixelData();
		if (pPixels == nullptr)
			return;
		if (!ensureFrameTexture(frame.nWidth, frame.nHeight, &strError))
		{
			emit renderError(strError);
			return;
		}
		const D3D11_BOX sourceBox = {
			0,
			0,
			0,
			static_cast<UINT>(frame.nWidth),
			static_cast<UINT>(frame.nHeight),
			1
		};
		m_spContext->UpdateSubresource(m_spFrameTexture.Get(),
			0,
			&sourceBox,
			pPixels,
			static_cast<UINT>(frame.nWidth * 4),
			0);
	}
	m_bI420Frame = bI420Frame;
	const qint64 nUploadSubmitUs = uploadTimer.nsecsElapsed() / 1000;
	QElapsedTimer presentTimer;
	presentTimer.start();

	render();
	const quint64 nGeneration = frame.spRenderSession ? frame.spRenderSession->nGeneration : 0;
	if (!m_bRenderPathLogged || m_lastLoggedPixelFormat != frame.pixelFormat
		|| m_nLoggedRenderGeneration != nGeneration || frame.nFrameIndex % 300 == 0)
	{
		KSessionTraceLogger::write(QStringLiteral("controller"), QStringLiteral("video_present"),
			m_bI420Frame ? QStringLiteral("i420_gpu") : QStringLiteral("bgra"), -1,
			QStringLiteral("uploadBytes=%1 uploadSubmitUs=%2 drawPresentCallUs=%3 generation=%4 requested=%5")
				.arg(nUploadBytes).arg(nUploadSubmitUs).arg(presentTimer.nsecsElapsed() / 1000).arg(nGeneration)
				.arg(g_bForceBgraVideoRenderer.load() ? QStringLiteral("bgra") : QStringLiteral("auto")));
		m_bRenderPathLogged = true;
		m_lastLoggedPixelFormat = frame.pixelFormat;
		m_nLoggedRenderGeneration = nGeneration;
	}
	const qint64 nPresentEndAtMs = SteadyNowMs();
	const KLatestDecodedFrameQueueStats renderStats =
		m_frameQueue.recordPresented();

	if (KLatencyTraceLogger::isEnabled()
		&& frame.nLastInputSeq > 0
		&& frame.nLastInputSeq > m_nLastRenderedInputSeq)
	{
		m_nLastRenderedInputSeq = frame.nLastInputSeq;
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("input_feedback_rendered"),
			QStringLiteral("seq=%1 frame=%2 sourceFrame=%3 inputAgeMs=%4 width=%5 height=%6")
				.arg(frame.nLastInputSeq)
				.arg(frame.nFrameIndex)
				.arg(frame.nSourceFrameIndex)
				.arg(frame.nInputAgeMs)
				.arg(frame.nWidth)
				.arg(frame.nHeight));
		emit inputFeedbackRendered(frame.nLastInputSeq);
	}

	if (bTraceFrame)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("render_end"),
			QStringLiteral("frame=%1 sourceFrame=%2 width=%3 height=%4 lowLatencyRender=%5 lastInputSeq=%6 inputAgeMs=%7 costMs=%8 callbackToConvertMs=%9 convertToEnqueueMs=%10 enqueueToPresentMs=%11 callbackToPresentMs=%12 renderReceivedTotal=%13 renderPresentedTotal=%14 renderCoalescedTotal=%15")
				.arg(frame.nFrameIndex)
				.arg(frame.nSourceFrameIndex)
				.arg(frame.nWidth)
				.arg(frame.nHeight)
				.arg(frame.bWebRtcLowLatencyRender ? 1 : 0)
				.arg(frame.nLastInputSeq)
				.arg(frame.nInputAgeMs)
				.arg(renderTimer.isValid() ? renderTimer.elapsed() : -1)
				.arg(frame.nRemoteCallbackAtMs >= 0 && frame.nConversionDoneAtMs >= 0
					? frame.nConversionDoneAtMs - frame.nRemoteCallbackAtMs : -1)
				.arg(frame.nConversionDoneAtMs >= 0
					? frame.nRenderEnqueuedAtMs - frame.nConversionDoneAtMs : -1)
				.arg(frame.nRenderEnqueuedAtMs >= 0
					? nPresentBeginAtMs - frame.nRenderEnqueuedAtMs : -1)
				.arg(frame.nRemoteCallbackAtMs >= 0
					? nPresentEndAtMs - frame.nRemoteCallbackAtMs : -1)
				.arg(renderStats.nReceivedFrames)
				.arg(renderStats.nPresentedFrames)
				.arg(renderStats.nCoalescedFrames));
	}
}

void KVideoRenderWidget::suspendRemoteInput()
{
	cancelPendingMouseMove();
	m_mouseClickTracker.clear();
	releaseRemoteKeys();
}

void KVideoRenderWidget::clearFrame()
{
	suspendRemoteInput();
	const KLatestDecodedFrameQueueStats renderStats = m_frameQueue.clear();
	m_i420Pipeline.clear();
	m_bI420Frame = false;
	m_bRenderPathLogged = false;
	m_spFrameShaderResourceView.Reset();
	m_spFrameTexture.Reset();
	m_nFrameWidth = 0;
	m_nFrameHeight = 0;
	m_nLastRenderedInputSeq = 0;
	if (renderStats.nReceivedFrames > 0 && KLatencyTraceLogger::isEnabled())
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("render_frame_coalesce_summary"),
			QStringLiteral("stage=gui_queue receivedTotal=%1 presentedTotal=%2 coalescedTotal=%3")
				.arg(renderStats.nReceivedFrames)
				.arg(renderStats.nPresentedFrames)
				.arg(renderStats.nCoalescedFrames));
	}

	if (!m_spContext || !m_spSwapChain || !m_spRenderTargetView)
		return;

	m_spContext->OMSetRenderTargets(1, m_spRenderTargetView.GetAddressOf(), nullptr);
	m_spContext->ClearRenderTargetView(m_spRenderTargetView.Get(), kClearColor);
	m_spSwapChain->Present(0, 0);
}

bool KVideoRenderWidget::event(QEvent *pEvent)
{
	if (pEvent != nullptr
		&& (pEvent->type() == QEvent::KeyPress || pEvent->type() == QEvent::KeyRelease))
	{
		QKeyEvent *pKeyEvent = static_cast<QKeyEvent *>(pEvent);
		if (pKeyEvent->key() == Qt::Key_Tab || pKeyEvent->key() == Qt::Key_Backtab)
		{
			if (m_bNativeKeyboardRoutingEnabled)
				return QWidget::event(pEvent);
			handleRemoteKeyEvent(pKeyEvent, pEvent->type() == QEvent::KeyPress);
			return true;
		}
	}

	const bool bHandled = QWidget::event(pEvent);
	if (pEvent && (pEvent->type() == QEvent::DevicePixelRatioChange
		|| pEvent->type() == QEvent::ScreenChangeInternal
		|| pEvent->type() == QEvent::WindowStateChange))
	{
		// Let Qt finish updating the native window before querying its client pixels.
		QTimer::singleShot(0, this, [this]() { render(); });
	}
	return bHandled;
}

void KVideoRenderWidget::resizeEvent(QResizeEvent *pEvent)
{
	QWidget::resizeEvent(pEvent);
	if (!m_spSwapChain)
		return;

	QString strError;
	if (!synchronizeRenderSize(&strError))
	{
		emit renderError(strError);
		return;
	}

	if (m_nFrameWidth > 0 && m_nFrameHeight > 0)
	{
		if (!updateVertexBuffer(m_nFrameWidth, m_nFrameHeight, &strError))
			emit renderError(strError);
		else
			render();
	}
}

void KVideoRenderWidget::showEvent(QShowEvent *pEvent)
{
	QWidget::showEvent(pEvent);
	QString strError;
	if (!initializeD3d(&strError))
		emit renderError(strError);
	else
		render();
}

void KVideoRenderWidget::hideEvent(QHideEvent *pEvent)
{
	m_mouseClickTracker.clear();
	releaseRemoteKeys();
	QWidget::hideEvent(pEvent);
}

void KVideoRenderWidget::focusOutEvent(QFocusEvent *pEvent)
{
	m_mouseClickTracker.clear();
	releaseRemoteKeys();
	emit keyboardCaptureFocusLost();
	QWidget::focusOutEvent(pEvent);
}

void KVideoRenderWidget::keyPressEvent(QKeyEvent *pEvent)
{
	if (m_bNativeKeyboardRoutingEnabled)
	{
		QWidget::keyPressEvent(pEvent);
		return;
	}
	handleRemoteKeyEvent(pEvent, true);
}

void KVideoRenderWidget::keyReleaseEvent(QKeyEvent *pEvent)
{
	if (m_bNativeKeyboardRoutingEnabled)
	{
		QWidget::keyReleaseEvent(pEvent);
		return;
	}
	handleRemoteKeyEvent(pEvent, false);
}

void KVideoRenderWidget::inputMethodEvent(QInputMethodEvent *pEvent)
{
	if (pEvent == nullptr)
		return;
	if (m_bNativeKeyboardRoutingEnabled)
	{
		QWidget::inputMethodEvent(pEvent);
		return;
	}
	m_bImeComposing = !pEvent->preeditString().isEmpty();
	if (!pEvent->commitString().isEmpty())
		emit remoteTextRequested(pEvent->commitString());
	pEvent->accept();
}

void KVideoRenderWidget::setNativeKeyboardRoutingEnabled(bool bEnabled)
{
	m_bNativeKeyboardRoutingEnabled = bEnabled;
	if (bEnabled)
		releaseRemoteKeys();
}

QPaintEngine *KVideoRenderWidget::paintEngine() const
{
	return nullptr;
}

void KVideoRenderWidget::setRemoteScreenSize(int nWidth, int nHeight)
{
	const int nRemoteScreenWidth = qMax(0, nWidth);
	const int nRemoteScreenHeight = qMax(0, nHeight);
	if (m_nRemoteScreenWidth != nRemoteScreenWidth
		|| m_nRemoteScreenHeight != nRemoteScreenHeight)
	{
		m_mouseClickTracker.clear();
	}
	m_nRemoteScreenWidth = nRemoteScreenWidth;
	m_nRemoteScreenHeight = nRemoteScreenHeight;
}

void KVideoRenderWidget::mouseMoveEvent(QMouseEvent *pEvent)
{
	QPoint remotePoint;
	bool bMapped = false;
	if (pEvent != nullptr)
	{
		bMapped = mapToRemotePoint(pEvent->position(), &remotePoint);
		if (!bMapped)
		{
			// Fast moves often overshoot the displayed frame rect (letterbox
			// margins or the widget edge). Clamp to the nearest edge and keep
			// sending so the remote cursor tracks to the screen edge instead
			// of freezing at the last in-rect position.
			bMapped = mapEdgeClampedRemotePoint(pEvent->position(), &remotePoint);
		}
	}
	if (bMapped)
	{
		m_pendingMouseMovePoint = remotePoint;
		m_bHasPendingMouseMove = true;
		const qint64 nElapsedMs = m_mouseMoveThrottleTimer.isValid()
			? m_mouseMoveThrottleTimer.elapsed()
			: kMouseMoveThrottleMs;
		if (nElapsedMs >= kMouseMoveThrottleMs)
		{
			flushPendingMouseMove();
		}
		else if (!m_mouseMoveFlushTimer.isActive())
			m_mouseMoveFlushTimer.start(static_cast<int>(
				kMouseMoveThrottleMs - nElapsedMs));
		pEvent->accept();
		return;
	}

	QWidget::mouseMoveEvent(pEvent);
}

void KVideoRenderWidget::mousePressEvent(QMouseEvent *pEvent)
{
	const int nButton = pEvent != nullptr ? qtMouseButtonToRemoteButton(pEvent->button()) : 0;
	QPoint remotePoint;
	if (nButton != 0 && pEvent != nullptr && mapToRemotePoint(pEvent->position(), &remotePoint))
	{
		cancelPendingMouseMove();
		setFocus(Qt::MouseFocusReason);
		emit keyboardCaptureRequested();
		emit remoteMouseInputAboutToSend();
		emit remoteMouseButtonRequested(remotePoint.x(), remotePoint.y(), nButton, true);
		pEvent->accept();
		return;
	}

	QWidget::mousePressEvent(pEvent);
}

void KVideoRenderWidget::mouseDoubleClickEvent(QMouseEvent *pEvent)
{
	const int nButton = pEvent != nullptr ? qtMouseButtonToRemoteButton(pEvent->button()) : 0;
	QPoint remotePoint;
	if (nButton != 0 && pEvent != nullptr && mapToRemotePoint(pEvent->position(), &remotePoint))
	{
		remotePoint = m_mouseClickTracker.beginDoubleClick(nButton, remotePoint);
		cancelPendingMouseMove();
		setFocus(Qt::MouseFocusReason);
		emit keyboardCaptureRequested();
		emit remoteMouseInputAboutToSend();
		emit remoteMouseButtonRequested(remotePoint.x(), remotePoint.y(), nButton, true);
		pEvent->accept();
		return;
	}

	QWidget::mouseDoubleClickEvent(pEvent);
}

void KVideoRenderWidget::mouseReleaseEvent(QMouseEvent *pEvent)
{
	const int nButton = pEvent != nullptr ? qtMouseButtonToRemoteButton(pEvent->button()) : 0;
	QPoint remotePoint;
	const bool bAnchoredDoubleClickRelease = nButton != 0
		&& m_mouseClickTracker.takeDoubleClickReleasePoint(nButton, &remotePoint);
	if (nButton != 0
		&& pEvent != nullptr
		&& (bAnchoredDoubleClickRelease
			|| mapToRemotePoint(pEvent->position(), &remotePoint)))
	{
		cancelPendingMouseMove();
		emit remoteMouseInputAboutToSend();
		emit remoteMouseButtonRequested(remotePoint.x(), remotePoint.y(), nButton, false);
		m_mouseClickTracker.recordCompletedClick(nButton, remotePoint);
		pEvent->accept();
		return;
	}

	QWidget::mouseReleaseEvent(pEvent);
}

void KVideoRenderWidget::wheelEvent(QWheelEvent *pEvent)
{
	QPoint remotePoint;
	if (pEvent != nullptr && mapToRemotePoint(pEvent->position(), &remotePoint))
	{
		const int nDelta = pEvent->angleDelta().y();
		if (nDelta != 0)
		{
			cancelPendingMouseMove();
			emit remoteMouseInputAboutToSend();
			emit remoteMouseWheelRequested(remotePoint.x(), remotePoint.y(), nDelta);
		}
		pEvent->accept();
		return;
	}

	QWidget::wheelEvent(pEvent);
}

void KVideoRenderWidget::leaveEvent(QEvent *pEvent)
{
	// No further move events arrive once the cursor leaves the widget, so
	// flush the latest pending position immediately.
	flushPendingMouseMove();
	QWidget::leaveEvent(pEvent);
}

void KVideoRenderWidget::flushPendingMouseMove()
{
	if (!m_bHasPendingMouseMove)
		return;

	m_mouseMoveFlushTimer.stop();
	const QPoint remotePoint = m_pendingMouseMovePoint;
	m_bHasPendingMouseMove = false;
	m_mouseMoveThrottleTimer.restart();
	emit remoteMouseMoveRequested(remotePoint.x(), remotePoint.y());
}

void KVideoRenderWidget::cancelPendingMouseMove()
{
	m_mouseMoveFlushTimer.stop();
	m_bHasPendingMouseMove = false;
}

void KVideoRenderWidget::handleRemoteKeyEvent(QKeyEvent *pEvent, bool bPressed)
{
	if (pEvent == nullptr)
		return;
	if (!bPressed && pEvent->isAutoRepeat())
	{
		pEvent->accept();
		return;
	}

	if (m_bImeComposing && !pEvent->text().isEmpty())
	{
		pEvent->accept();
		return;
	}

	const quint32 nNativeScanCode = pEvent->nativeScanCode();
	const int nVirtualKey = normalizeVirtualKey(
		static_cast<int>(pEvent->nativeVirtualKey()), nNativeScanCode);
	if (nVirtualKey <= 0 || nVirtualKey > 0xFF)
	{
		if (bPressed)
			QWidget::keyPressEvent(pEvent);
		else
			QWidget::keyReleaseEvent(pEvent);
		return;
	}

	const bool bExtended = isExtendedVirtualKey(nVirtualKey, nNativeScanCode);
	const int nScanCode = static_cast<int>(nNativeScanCode & 0x1FF);
	const quint32 nKeyId = static_cast<quint32>(nVirtualKey)
		| (static_cast<quint32>(nScanCode) << kScanCodeShift)
		| (bExtended ? kExtendedKeyMask : 0);
	if (bPressed)
		m_pressedRemoteKeys.insert(nKeyId);
	else
		m_pressedRemoteKeys.remove(nKeyId);

	emit remoteKeyRequested(nVirtualKey, nScanCode, bPressed, bExtended, pEvent->isAutoRepeat());
	pEvent->accept();
}

void KVideoRenderWidget::releaseRemoteKeys()
{
	const QSet<quint32> pressedKeys = m_pressedRemoteKeys;
	m_pressedRemoteKeys.clear();
	for (const quint32 nKeyId : pressedKeys)
	{
		emit remoteKeyRequested(static_cast<int>(nKeyId & 0xFF),
			static_cast<int>((nKeyId >> kScanCodeShift) & 0x1FF),
			false,
			(nKeyId & kExtendedKeyMask) != 0,
			false);
	}
}

int KVideoRenderWidget::normalizeVirtualKey(int nVirtualKey, quint32 nNativeScanCode)
{
	if (nVirtualKey == VK_SHIFT)
	{
		const UINT nMapped = MapVirtualKeyW(nNativeScanCode & 0xFF, MAPVK_VSC_TO_VK_EX);
		return nMapped == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
	}
	if (nVirtualKey == VK_CONTROL)
		return (nNativeScanCode & 0x100) != 0 ? VK_RCONTROL : VK_LCONTROL;
	if (nVirtualKey == VK_MENU)
		return (nNativeScanCode & 0x100) != 0 ? VK_RMENU : VK_LMENU;
	return nVirtualKey;
}

bool KVideoRenderWidget::isExtendedVirtualKey(int nVirtualKey, quint32 nNativeScanCode)
{
	if ((nNativeScanCode & 0x100) != 0)
		return true;

	switch (nVirtualKey)
	{
	case VK_RCONTROL:
	case VK_RMENU:
	case VK_CANCEL:
	case VK_INSERT:
	case VK_DELETE:
	case VK_HOME:
	case VK_END:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_LEFT:
	case VK_RIGHT:
	case VK_UP:
	case VK_DOWN:
	case VK_NUMLOCK:
	case VK_SNAPSHOT:
	case VK_DIVIDE:
	case VK_LWIN:
	case VK_RWIN:
	case VK_APPS:
		return true;
	default:
		return false;
	}
}

QSize KVideoRenderWidget::physicalClientSize() const
{
	const HWND hWindow = reinterpret_cast<HWND>(effectiveWinId());
	if (!hWindow || !IsWindow(hWindow) || IsIconic(GetAncestor(hWindow, GA_ROOT)))
		return QSize();
	// Query in the HWND's own awareness context; never change process DPI policy.
	const DPI_AWARENESS_CONTEXT windowContext = GetWindowDpiAwarenessContext(hWindow);
	const DPI_AWARENESS_CONTEXT previousContext = SetThreadDpiAwarenessContext(windowContext);
	RECT clientRect = {};
	const BOOL bResult = GetClientRect(hWindow, &clientRect);
	if (previousContext)
		SetThreadDpiAwarenessContext(previousContext);
	if (!bResult)
		return QSize();
	return QSize(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
}

bool KVideoRenderWidget::synchronizeRenderSize(QString *pErrorMessage)
{
	const QSize pixelSize = physicalClientSize();
	if (pixelSize.isEmpty() || !m_spSwapChain)
		return true;
	if (pixelSize == m_renderPixelSize && m_spRenderTargetView)
		return true;
	m_spContext->OMSetRenderTargets(0, nullptr, nullptr);
	releaseRenderTarget();
	const HRESULT hrResize = m_spSwapChain->ResizeBuffers(0,
		static_cast<UINT>(pixelSize.width()), static_cast<UINT>(pixelSize.height()),
		DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hrResize))
	{
		if (pErrorMessage)
			*pErrorMessage = hresultMessage(QStringLiteral("Resize D3D11 swap chain failed"), hrResize);
		return false;
	}
	return createRenderTarget(pErrorMessage);
}

bool KVideoRenderWidget::initializeD3d(QString *pErrorMessage)
{
	if (m_bInitialized)
		return true;

	const QSize widgetSize = physicalClientSize();
	DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
	swapChainDesc.BufferDesc.Width = static_cast<UINT>(qMax(1, widgetSize.width()));
	swapChainDesc.BufferDesc.Height = static_cast<UINT>(qMax(1, widgetSize.height()));
	swapChainDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.OutputWindow = reinterpret_cast<HWND>(winId());
	swapChainDesc.Windowed = TRUE;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0
	};
	D3D_FEATURE_LEVEL createdFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	const HRESULT hrCreate = D3D11CreateDeviceAndSwapChain(nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		featureLevels,
		static_cast<UINT>(std::size(featureLevels)),
		D3D11_SDK_VERSION,
		&swapChainDesc,
		&m_spSwapChain,
		&m_spDevice,
		&createdFeatureLevel,
		&m_spContext);
	if (FAILED(hrCreate))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 device failed"), hrCreate);
		return false;
	}

	if (!createRenderTarget(pErrorMessage) ||
		!createShaders(pErrorMessage) ||
		!createSampler(pErrorMessage))
	{
		releaseAll();
		return false;
	}

	D3D11_BUFFER_DESC vertexBufferDesc = {};
	vertexBufferDesc.ByteWidth = sizeof(KVertex) * 4;
	vertexBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vertexBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	const HRESULT hrVertexBuffer = m_spDevice->CreateBuffer(&vertexBufferDesc, nullptr, &m_spVertexBuffer);
	if (FAILED(hrVertexBuffer))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 vertex buffer failed"), hrVertexBuffer);
		releaseAll();
		return false;
	}

	m_bInitialized = true;
	return true;
}

bool KVideoRenderWidget::createRenderTarget(QString *pErrorMessage)
{
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spBackBuffer;
	const HRESULT hrBuffer = m_spSwapChain->GetBuffer(0, IID_PPV_ARGS(&spBackBuffer));
	if (FAILED(hrBuffer))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Get D3D11 back buffer failed"), hrBuffer);
		return false;
	}

	const HRESULT hrRenderTarget = m_spDevice->CreateRenderTargetView(spBackBuffer.Get(),
		nullptr,
		&m_spRenderTargetView);
	if (FAILED(hrRenderTarget))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 render target failed"), hrRenderTarget);
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	spBackBuffer->GetDesc(&desc);
	m_renderPixelSize = QSize(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
	const HWND hWindow = reinterpret_cast<HWND>(winId());
	KSessionTraceLogger::write(QStringLiteral("controller"), QStringLiteral("render_size"),
		QStringLiteral("logical=%1x%2 dpr=%3 client=%4x%5 buffer=%6x%7 windowDpi=%8 awareness=%9 frame=%10x%11")
			.arg(width()).arg(height()).arg(devicePixelRatioF())
			.arg(physicalClientSize().width()).arg(physicalClientSize().height())
			.arg(desc.Width).arg(desc.Height).arg(GetDpiForWindow(hWindow))
			.arg(static_cast<int>(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hWindow))))
			.arg(m_nFrameWidth).arg(m_nFrameHeight));
	return true;
}

bool KVideoRenderWidget::createShaders(QString *pErrorMessage)
{
	Microsoft::WRL::ComPtr<ID3DBlob> spVertexBlob;
	Microsoft::WRL::ComPtr<ID3DBlob> spErrorBlob;
	HRESULT hrCompile = D3DCompile(kVertexShaderSource,
		std::strlen(kVertexShaderSource),
		nullptr,
		nullptr,
		nullptr,
		"main",
		"vs_4_0",
		0,
		0,
		&spVertexBlob,
		&spErrorBlob);
	if (FAILED(hrCompile))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Compile D3D11 vertex shader failed"), hrCompile);
		return false;
	}

	const HRESULT hrVertexShader = m_spDevice->CreateVertexShader(spVertexBlob->GetBufferPointer(),
		spVertexBlob->GetBufferSize(),
		nullptr,
		&m_spVertexShader);
	if (FAILED(hrVertexShader))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 vertex shader failed"), hrVertexShader);
		return false;
	}

	const D3D11_INPUT_ELEMENT_DESC inputElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
	};
	const HRESULT hrInputLayout = m_spDevice->CreateInputLayout(inputElements,
		static_cast<UINT>(std::size(inputElements)),
		spVertexBlob->GetBufferPointer(),
		spVertexBlob->GetBufferSize(),
		&m_spInputLayout);
	if (FAILED(hrInputLayout))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 input layout failed"), hrInputLayout);
		return false;
	}

	Microsoft::WRL::ComPtr<ID3DBlob> spPixelBlob;
	hrCompile = D3DCompile(kPixelShaderSource,
		std::strlen(kPixelShaderSource),
		nullptr,
		nullptr,
		nullptr,
		"main",
		"ps_4_0",
		0,
		0,
		&spPixelBlob,
		&spErrorBlob);
	if (FAILED(hrCompile))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Compile D3D11 pixel shader failed"), hrCompile);
		return false;
	}

	const HRESULT hrPixelShader = m_spDevice->CreatePixelShader(spPixelBlob->GetBufferPointer(),
		spPixelBlob->GetBufferSize(),
		nullptr,
		&m_spPixelShader);
	if (FAILED(hrPixelShader))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 pixel shader failed"), hrPixelShader);
		return false;
	}

	return true;
}

bool KVideoRenderWidget::createSampler(QString *pErrorMessage)
{
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

	const HRESULT hrSampler = m_spDevice->CreateSamplerState(&samplerDesc, &m_spSamplerState);
	if (FAILED(hrSampler))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 sampler failed"), hrSampler);
		return false;
	}

	return true;
}

bool KVideoRenderWidget::ensureFrameTexture(int nWidth, int nHeight, QString *pErrorMessage)
{
	if (m_spFrameTexture)
	{
		D3D11_TEXTURE2D_DESC current = {};
		m_spFrameTexture->GetDesc(&current);
		if (current.Width == static_cast<UINT>(nWidth) && current.Height == static_cast<UINT>(nHeight))
		{
			m_nFrameWidth = nWidth;
			m_nFrameHeight = nHeight;
			return true;
		}
	}

	m_spFrameShaderResourceView.Reset();
	m_spFrameTexture.Reset();

	D3D11_TEXTURE2D_DESC textureDesc = {};
	textureDesc.Width = static_cast<UINT>(nWidth);
	textureDesc.Height = static_cast<UINT>(nHeight);
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	const HRESULT hrTexture = m_spDevice->CreateTexture2D(&textureDesc, nullptr, &m_spFrameTexture);
	if (FAILED(hrTexture))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 frame texture failed"), hrTexture);
		return false;
	}

	const HRESULT hrView = m_spDevice->CreateShaderResourceView(m_spFrameTexture.Get(),
		nullptr,
		&m_spFrameShaderResourceView);
	if (FAILED(hrView))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create D3D11 frame texture view failed"), hrView);
		return false;
	}

	m_nFrameWidth = nWidth;
	m_nFrameHeight = nHeight;
	return true;
}

bool KVideoRenderWidget::updateVertexBuffer(int nFrameWidth, int nFrameHeight, QString *pErrorMessage)
{
	if (!m_spVertexBuffer)
		return false;

	const QSize widgetSize = size();
	const float fWidgetWidth = static_cast<float>(qMax(1, widgetSize.width()));
	const float fWidgetHeight = static_cast<float>(qMax(1, widgetSize.height()));
	const float fFrameAspect = static_cast<float>(nFrameWidth) / static_cast<float>(nFrameHeight);
	const float fWidgetAspect = fWidgetWidth / fWidgetHeight;

	float fScaleX = 1.0f;
	float fScaleY = 1.0f;
	if (fWidgetAspect > fFrameAspect)
		fScaleX = fFrameAspect / fWidgetAspect;
	else
		fScaleY = fWidgetAspect / fFrameAspect;

	const float fDisplayWidth = fWidgetWidth * fScaleX;
	const float fDisplayHeight = fWidgetHeight * fScaleY;
	m_frameDisplayRect = QRectF((fWidgetWidth - fDisplayWidth) * 0.5f,
		(fWidgetHeight - fDisplayHeight) * 0.5f,
		fDisplayWidth,
		fDisplayHeight);

	const KVertex vertices[4] = {
		{ -fScaleX, fScaleY, 0.0f, 0.0f },
		{ fScaleX, fScaleY, 1.0f, 0.0f },
		{ -fScaleX, -fScaleY, 0.0f, 1.0f },
		{ fScaleX, -fScaleY, 1.0f, 1.0f }
	};

	D3D11_MAPPED_SUBRESOURCE mappedResource = {};
	const HRESULT hrMap = m_spContext->Map(m_spVertexBuffer.Get(),
		0,
		D3D11_MAP_WRITE_DISCARD,
		0,
		&mappedResource);
	if (FAILED(hrMap))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Map D3D11 vertex buffer failed"), hrMap);
		return false;
	}

	std::memcpy(mappedResource.pData, vertices, sizeof(vertices));
	m_spContext->Unmap(m_spVertexBuffer.Get(), 0);
	return true;
}

void KVideoRenderWidget::releaseRenderTarget()
{
	m_spRenderTargetView.Reset();
}

void KVideoRenderWidget::releaseAll()
{
	m_i420Pipeline.clear();
	m_bI420Frame = false;
	releaseRenderTarget();
	m_spVertexBuffer.Reset();
	m_spInputLayout.Reset();
	m_spPixelShader.Reset();
	m_spVertexShader.Reset();
	m_spSamplerState.Reset();
	m_spFrameShaderResourceView.Reset();
	m_spFrameTexture.Reset();
	m_spSwapChain.Reset();
	m_spContext.Reset();
	m_spDevice.Reset();
	m_bInitialized = false;
	m_nFrameWidth = 0;
	m_nFrameHeight = 0;
	m_frameDisplayRect = QRectF();
}

void KVideoRenderWidget::render()
{
	if (!isVisible() || physicalClientSize().isEmpty())
		return;
	QString strError;
	if (!synchronizeRenderSize(&strError))
	{
		emit renderError(strError);
		return;
	}
	if (!m_spContext || !m_spSwapChain || !m_spRenderTargetView
		|| (m_bI420Frame ? !m_i420Pipeline.isReady() : !m_spFrameShaderResourceView))
		return;
	if (!updateVertexBuffer(m_nFrameWidth, m_nFrameHeight, &strError))
	{
		emit renderError(strError);
		return;
	}

	const QSize widgetSize = m_renderPixelSize;
	D3D11_VIEWPORT viewport = {};
	viewport.Width = static_cast<float>(qMax(1, widgetSize.width()));
	viewport.Height = static_cast<float>(qMax(1, widgetSize.height()));
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	UINT nStride = sizeof(KVertex);
	UINT nOffset = 0;
	ID3D11Buffer *pVertexBuffer = m_spVertexBuffer.Get();
	ID3D11ShaderResourceView *pShaderResourceView = m_spFrameShaderResourceView.Get();
	ID3D11SamplerState *pSamplerState = m_spSamplerState.Get();

	m_spContext->OMSetRenderTargets(1, m_spRenderTargetView.GetAddressOf(), nullptr);
	m_spContext->RSSetViewports(1, &viewport);
	m_spContext->ClearRenderTargetView(m_spRenderTargetView.Get(), kClearColor);
	m_spContext->IASetInputLayout(m_spInputLayout.Get());
	m_spContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_spContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &nStride, &nOffset);
	m_spContext->VSSetShader(m_spVertexShader.Get(), nullptr, 0);
	m_spContext->PSSetShader(m_spPixelShader.Get(), nullptr, 0);
	m_spContext->PSSetShaderResources(0, 1, &pShaderResourceView);
	if (m_bI420Frame)
		m_i420Pipeline.bind(m_spContext.Get());
	m_spContext->PSSetSamplers(0, 1, &pSamplerState);
	m_spContext->Draw(4, 0);
	ID3D11ShaderResourceView *emptyViews[3] = {};
	m_spContext->PSSetShaderResources(0, 3, emptyViews);
	m_spSwapChain->Present(0, 0);
}

bool KVideoRenderWidget::mapToRemotePoint(const QPointF &localPoint, QPoint *pRemotePoint) const
{
	if (pRemotePoint == nullptr || m_nFrameWidth <= 0 || m_nFrameHeight <= 0 || m_frameDisplayRect.isEmpty())
		return false;
	if (!m_frameDisplayRect.contains(localPoint))
		return false;

	const int nTargetWidth = m_nRemoteScreenWidth > 0 ? m_nRemoteScreenWidth : m_nFrameWidth;
	const int nTargetHeight = m_nRemoteScreenHeight > 0 ? m_nRemoteScreenHeight : m_nFrameHeight;
	const double fRelativeX = (localPoint.x() - m_frameDisplayRect.left()) / m_frameDisplayRect.width();
	const double fRelativeY = (localPoint.y() - m_frameDisplayRect.top()) / m_frameDisplayRect.height();
	const int nRemoteX = std::clamp(static_cast<int>(fRelativeX * nTargetWidth), 0, nTargetWidth - 1);
	const int nRemoteY = std::clamp(static_cast<int>(fRelativeY * nTargetHeight), 0, nTargetHeight - 1);
	*pRemotePoint = QPoint(nRemoteX, nRemoteY);
	return true;
}

bool KVideoRenderWidget::mapEdgeClampedRemotePoint(const QPointF &localPoint, QPoint *pRemotePoint) const
{
	if (m_frameDisplayRect.isEmpty())
		return false;

	const QPointF clampedPoint(
		std::clamp(localPoint.x(), m_frameDisplayRect.left(), m_frameDisplayRect.right()),
		std::clamp(localPoint.y(), m_frameDisplayRect.top(), m_frameDisplayRect.bottom()));
	return mapToRemotePoint(clampedPoint, pRemotePoint);
}

int KVideoRenderWidget::qtMouseButtonToRemoteButton(Qt::MouseButton button)
{
	if (button == Qt::LeftButton)
		return kRemoteMouseButtonLeft;
	if (button == Qt::RightButton)
		return kRemoteMouseButtonRight;
	if (button == Qt::MiddleButton)
		return kRemoteMouseButtonMiddle;
	if (button == Qt::BackButton)
		return kRemoteMouseButtonX1;
	if (button == Qt::ForwardButton)
		return kRemoteMouseButtonX2;
	return 0;
}

QString KVideoRenderWidget::hresultMessage(const QString &strPrefix, HRESULT hr)
{
	return QStringLiteral("%1: HRESULT 0x%2")
		.arg(strPrefix)
		.arg(static_cast<unsigned int>(hr), 8, 16, QLatin1Char('0'));
}
