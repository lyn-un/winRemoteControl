// Device creation only. Never initialize or acquire desktop duplication.
#include "capture/dxgidesktopduplicator.h"

#include <QtCore/QCoreApplication>

#include <iostream>

struct KHdrCaptureTestAccess
{
	static bool initializeDevice(KDxgiDesktopDuplicator *pCapture, QString *pError)
	{
		return pCapture->initializeDevice(pError);
	}
	static ID3D11Device *device(KDxgiDesktopDuplicator *pCapture)
	{
		return pCapture->m_spDevice.Get();
	}
};

int main(int nArgc, char **pArgv)
{
	QCoreApplication application(nArgc, pArgv);
	if (qEnvironmentVariableIntValue("WRC_RUN_GPU_FRAME_TEST") != 1)
	{
		std::cout << "Set WRC_RUN_GPU_FRAME_TEST=1 for hardware device initialization" << std::endl;
		return 77;
	}
	QString strError;
	KDxgiDesktopDuplicator preview;
	if (!KHdrCaptureTestAccess::initializeDevice(&preview, &strError))
	{
		std::cerr << strError.toStdString() << std::endl;
		return 1;
	}
	Microsoft::WRL::ComPtr<ID3D11Device> spPreviewDevice = KHdrCaptureTestAccess::device(&preview);
	if (spPreviewDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)
		return 1;
	auto spPath = std::make_shared<KVideoFramePathState>(GpuVideoFramePath);
	spPath->begin(1);
	spPath->setSeparateCursor(1, true);
	KDxgiDesktopDuplicator capture(AutoHdrConversionMode, spPath);
	if (!KHdrCaptureTestAccess::initializeDevice(&capture, &strError))
	{
		std::cerr << strError.toStdString() << std::endl;
		return 1;
	}
	ID3D11Device *pVideoDevice = KHdrCaptureTestAccess::device(&capture);
	if (pVideoDevice == spPreviewDevice.Get()
		|| !(pVideoDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)
		|| KHdrCaptureTestAccess::device(&preview) != spPreviewDevice.Get())
	{
		return 1;
	}
	// Existing owners must still be able to allocate on the old device.
	D3D11_TEXTURE2D_DESC descriptor = {};
	descriptor.Width = descriptor.Height = 16;
	descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
	descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spTexture;
	if (FAILED(spPreviewDevice->CreateTexture2D(&descriptor, nullptr, &spTexture)))
		return 1;
	preview.shutdown();
	KDxgiDesktopDuplicator nextPreview;
	if (!KHdrCaptureTestAccess::initializeDevice(&nextPreview, &strError)
		|| KHdrCaptureTestAccess::device(&nextPreview) != pVideoDevice)
	{
		return 1;
	}
	std::cout << "preview -> video device upgrade, old ownership and cache reuse passed; no desktop acquired"
		<< std::endl;
	return 0;
}
