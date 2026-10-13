#include "render/videorenderwidget.h"

#include <QtWidgets/QApplication>
#include <QtCore/QThread>
#include <iostream>

class KVideoRenderWidgetTestAccess
{
public:
	static bool verify(KVideoRenderWidget &widget)
	{
		if (!widget.m_spSwapChain)
			return false;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> spBuffer;
		if (FAILED(widget.m_spSwapChain->GetBuffer(0, IID_PPV_ARGS(&spBuffer))))
			return false;
		D3D11_TEXTURE2D_DESC desc = {};
		spBuffer->GetDesc(&desc);
		const QSize actual(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
		D3D11_VIEWPORT viewport = {};
		UINT nCount = 1;
		widget.m_spContext->RSGetViewports(&nCount, &viewport);
		QPoint remotePoint;
		const bool bCenter = widget.mapToRemotePoint(widget.m_frameDisplayRect.center(), &remotePoint)
			&& (remotePoint - QPoint(1280, 720)).manhattanLength() <= 2;
		const bool bCorner = widget.mapToRemotePoint(widget.m_frameDisplayRect.topLeft(), &remotePoint)
			&& remotePoint == QPoint(0, 0);
		const bool bOutside = !widget.mapToRemotePoint(widget.m_frameDisplayRect.topLeft() - QPointF(1, 1), &remotePoint);
		bool bEdges = true;
		const QRectF display = widget.m_frameDisplayRect;
		const double fInsetX = display.width() / 2560.0 * 0.25;
		const double fInsetY = display.height() / 1440.0 * 0.25;
		for (int nCorner = 0; nCorner < 4; ++nCorner)
		{
			const QPointF point(nCorner & 1 ? display.right() - fInsetX : display.left() + fInsetX,
				nCorner & 2 ? display.bottom() - fInsetY : display.top() + fInsetY);
			const QPoint expected(nCorner & 1 ? 2559 : 0, nCorner & 2 ? 1439 : 0);
			bEdges &= widget.mapToRemotePoint(point, &remotePoint) && remotePoint == expected;
		}
		std::cout << "logical=" << widget.width() << 'x' << widget.height()
			<< " dpr=" << widget.devicePixelRatioF() << " buffer=" << desc.Width << 'x' << desc.Height << '\n';
		return actual == widget.physicalClientSize() && actual == widget.m_renderPixelSize
			&& nCount == 1 && viewport.Width == desc.Width && viewport.Height == desc.Height
			&& bCenter && bCorner && bOutside && bEdges;
	}

	static bool verifyMinimized(KVideoRenderWidget &widget)
	{
		const QSize previous = widget.m_renderPixelSize;
		widget.render();
		return widget.physicalClientSize().isEmpty() && widget.m_renderPixelSize == previous;
	}

	static bool invalidateBuffer(KVideoRenderWidget &widget)
	{
		// Simulate a stale backing buffer without receiving another remote frame.
		widget.m_spContext->OMSetRenderTargets(0, nullptr, nullptr);
		widget.releaseRenderTarget();
		const HRESULT hr = widget.m_spSwapChain->ResizeBuffers(0, 16, 16, DXGI_FORMAT_UNKNOWN, 0);
		QString strError;
		return SUCCEEDED(hr) && widget.createRenderTarget(&strError);
	}
};

static void ProcessEvents()
{
	for (int nIndex = 0; nIndex < 10; ++nIndex)
	{
		QApplication::processEvents();
		QThread::msleep(10);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1)
		qputenv("QT_SCALE_FACTOR", argv[1]);
	QApplication application(argc, argv);
	KVideoRenderWidget widget;
	bool bSuccess = true;
	QObject::connect(&widget, &KVideoRenderWidget::renderError,
		[&](const QString &message) { std::cerr << message.toStdString() << '\n'; bSuccess = false; });
	widget.resize(641, 479);
	widget.show();
	ProcessEvents();
	widget.setRemoteScreenSize(2560, 1440);
	KDecodedVideoFrame frame;
	frame.nWidth = 256;
	frame.nHeight = 144;
	frame.spBgraBuffer = std::make_shared<std::vector<unsigned char>>(256 * 144 * 4, 255);
	widget.presentFrame(frame);
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	for (const QSize size : {QSize(799, 601), QSize(320, 720), QSize(960, 540)})
	{
		widget.resize(size);
		ProcessEvents();
		bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	}
	widget.showMinimized();
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verifyMinimized(widget);
	widget.showNormal();
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	widget.showMaximized();
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	widget.showNormal();
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	frame.pixelFormat = KDecodedPixelFormat::I420;
	auto spPlanes = std::make_shared<std::array<std::vector<unsigned char>, 3>>();
	for (int nPlane = 0; nPlane < 3; ++nPlane)
	{
		const int nWidth = nPlane ? 128 : 256;
		const int nHeight = nPlane ? 72 : 144;
		(*spPlanes)[nPlane].resize(nWidth * nHeight, nPlane ? 128 : 180);
		frame.planes[nPlane] = {(*spPlanes)[nPlane].data(), nWidth, nHeight, nWidth,
			(*spPlanes)[nPlane].size()};
	}
	frame.spPlaneOwner = spPlanes;
	widget.presentFrame(frame);
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	widget.resize(701, 503);
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	bSuccess &= KVideoRenderWidgetTestAccess::invalidateBuffer(widget);
	QEvent dpiEvent(QEvent::DevicePixelRatioChange);
	QApplication::sendEvent(&widget, &dpiEvent);
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	widget.showFullScreen();
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	widget.showNormal();
	ProcessEvents();
	bSuccess &= KVideoRenderWidgetTestAccess::verify(widget);
	widget.close();
	return bSuccess ? 0 : 1;
}
