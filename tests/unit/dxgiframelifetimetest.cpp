#include "capture/dxgidesktopduplicator.h"

#include <iostream>

namespace
{
class KNonTextureResource final : public IDXGIResource
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **ppObject) override
	{
		if (!ppObject)
			return E_POINTER;
		*ppObject = nullptr;
		if (id != __uuidof(IUnknown) && id != __uuidof(IDXGIObject)
			&& id != __uuidof(IDXGIDeviceSubObject) && id != __uuidof(IDXGIResource))
			return E_NOINTERFACE;
		*ppObject = static_cast<IDXGIResource *>(this);
		AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_nReferences; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG nRemaining = --m_nReferences;
		if (!nRemaining)
			delete this;
		return nRemaining;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT *) override { return E_NOTIMPL; }

private:
	ULONG m_nReferences = 1;
};

class KFakeDuplication final : public IDXGIOutputDuplication
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **ppObject) override
	{
		if (!ppObject)
			return E_POINTER;
		*ppObject = nullptr;
		if (id != __uuidof(IUnknown) && id != __uuidof(IDXGIObject)
			&& id != __uuidof(IDXGIOutputDuplication))
			return E_NOINTERFACE;
		*ppObject = static_cast<IDXGIOutputDuplication *>(this);
		AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_nReferences; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG nRemaining = --m_nReferences;
		if (!nRemaining)
			delete this;
		return nRemaining;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override { return E_NOTIMPL; }
	void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC *pDesc) override { if (pDesc) *pDesc = {}; }
	HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT, DXGI_OUTDUPL_FRAME_INFO *pInfo,
		IDXGIResource **ppResource) override
	{
		++nAcquireCalls;
		*pInfo = {};
		*ppResource = nullptr;
		if (bHeld)
			return DXGI_ERROR_INVALID_CALL;
		if (FAILED(hrAcquire))
			return hrAcquire;
		bHeld = true;
		pInfo->PointerShapeBufferSize = bPointerFailure ? 16 : 0;
		if (bNonTextureResource)
		{
			pInfo->AccumulatedFrames = 1;
			*ppResource = new KNonTextureResource();
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT, RECT *, UINT *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT, DXGI_OUTDUPL_MOVE_RECT *, UINT *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT, void *, UINT *, DXGI_OUTDUPL_POINTER_SHAPE_INFO *) override
	{
		return E_FAIL;
	}
	HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT *) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE ReleaseFrame() override
	{
		++nReleaseCalls;
		if (!bHeld)
			return DXGI_ERROR_INVALID_CALL;
		if (FAILED(hrRelease))
			return hrRelease;
		bHeld = false;
		return S_OK;
	}
	HRESULT hrAcquire = S_OK;
	HRESULT hrRelease = S_OK;
	bool bPointerFailure = false;
	bool bNonTextureResource = false;
	bool bHeld = false;
	int nAcquireCalls = 0;
	int nReleaseCalls = 0;

private:
	ULONG m_nReferences = 1;
};
}

struct KHdrCaptureTestAccess
{
	static void install(KDxgiDesktopDuplicator *pCapture, IDXGIOutputDuplication *pDuplication)
	{
		pCapture->m_spDuplication.Attach(pDuplication);
		pCapture->m_nSourceGeneration = 1;
	}
};

int main()
{
	for (int nCase = 0; nCase < 7; ++nCase)
	{
		auto spPath = std::make_shared<KVideoFramePathState>(GpuVideoFramePath);
		spPath->begin(1);
		// Legacy composition propagates shape-read errors. Separated cursors
		// intentionally tolerate them by publishing the default cursor.
		spPath->setSeparateCursor(1, nCase < 3 || nCase >= 5);
		KDxgiDesktopDuplicator capture(AutoHdrConversionMode, spPath);
		auto *pDuplication = new KFakeDuplication();
		KHdrCaptureTestAccess::install(&capture, pDuplication);
		if (nCase == 1)
			pDuplication->hrAcquire = DXGI_ERROR_WAIT_TIMEOUT;
		if (nCase == 2 || nCase == 4 || nCase == 6)
			pDuplication->hrRelease = DXGI_ERROR_ACCESS_LOST;
		if (nCase == 3 || nCase == 4)
			pDuplication->bPointerFailure = true;
		pDuplication->bNonTextureResource = nCase >= 5;
		const int nIterations = nCase == 0 || nCase == 5 ? 200 : 1;
		for (int nIndex = 0; nIndex < nIterations; ++nIndex)
		{
			KCaptureFrame frame;
			QString strError;
			const auto result = capture.captureNextFrame(&frame, &strError);
			const bool bExpectedError = nCase >= 2;
			if (result != (bExpectedError ? IKCaptureSource::ErrorCaptureResult : IKCaptureSource::TimeoutCaptureResult)
				|| pDuplication->nReleaseCalls != (nCase == 1 ? 0 : nIndex + 1))
			{
				std::cerr << "Unexpected capture/release result case=" << nCase << " iteration=" << nIndex
					<< " result=" << result << " releases=" << pDuplication->nReleaseCalls
					<< " detail=" << strError.toStdString() << std::endl;
				return 1;
			}
			if ((nCase == 2 || nCase == 4 || nCase == 6)
				&& (!strError.contains(QStringLiteral("ReleaseFrame failed"))
					|| !strError.contains(QStringLiteral("0x887a0026"))))
				return 1;
			if ((nCase == 3 || nCase == 4)
				&& !strError.contains(QStringLiteral("GetFramePointerShape failed")))
				return 1;
			if (nCase >= 5 && !strError.contains(QStringLiteral("Query desktop texture failed")))
				return 1;
			if ((nCase == 0 || nCase == 5) && pDuplication->bHeld)
				return 1;
		}
		if (nCase == 0)
		{
			pDuplication->hrAcquire = DXGI_ERROR_INVALID_CALL;
			KCaptureFrame frame;
			QString strError;
			if (capture.captureNextFrame(&frame, &strError) != IKCaptureSource::ErrorCaptureResult
				|| !strError.contains(QStringLiteral("acquired=200 released=200 lastRelease=0x00000000"))
				|| pDuplication->nReleaseCalls != 200)
				return 1;
		}
	}
	std::cout << "DXGI fake: 200 balanced frames, 200 texture-query failures with balanced releases, "
		"timeout without release, release failure, prior error "
		"preservation and acquire diagnostics passed; no desktop or driver accessed" << std::endl;
	return 0;
}
