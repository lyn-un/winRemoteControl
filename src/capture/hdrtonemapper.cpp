#include "capture/hdrtonemapper.h"

#include <d2d1effects_2.h>
#include <dxgi.h>
#include <cmath>

using Microsoft::WRL::ComPtr;

void KHdrToneMapper::reset()
{
	if (m_spContext)
		m_spContext->SetTarget(nullptr);
	m_spOutputBitmap.Reset();
	m_spInputBitmap.Reset();
	m_spWhiteAdjust.Reset();
	m_spToneMap.Reset();
	m_spContext.Reset();
	m_spInput.Reset();
	m_spOutput.Reset();
	m_spDevice.Reset();
	m_nWidth = m_nHeight = 0;
}

HRESULT KHdrToneMapper::initialize(ID3D11Device *pDevice)
{
	reset();
	ComPtr<IDXGIDevice> spDxgiDevice;
	HRESULT hr = pDevice->QueryInterface(IID_PPV_ARGS(&spDxgiDevice));
	if (FAILED(hr))
		return hr;
	ComPtr<ID2D1Factory1> spFactory;
	D2D1_FACTORY_OPTIONS options = {};
	hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
		&options, reinterpret_cast<void **>(spFactory.GetAddressOf()));
	if (FAILED(hr))
		return hr;
	ComPtr<ID2D1Device> spDevice;
	hr = spFactory->CreateDevice(spDxgiDevice.Get(), &spDevice);
	if (FAILED(hr))
		return hr;
	hr = spDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_spContext);
	if (FAILED(hr))
		return hr;
	m_spContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
	m_spContext->SetRenderingControls(D2D1::RenderingControls(
		D2D1_BUFFER_PRECISION_16BPC_FLOAT, D2D1::SizeU(0, 0)));
	hr = m_spContext->CreateEffect(CLSID_D2D1HdrToneMap, &m_spToneMap);
	if (FAILED(hr))
		return hr;
	hr = m_spContext->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, &m_spWhiteAdjust);
	if (FAILED(hr))
		return hr;
	m_spWhiteAdjust->SetInputEffect(0, m_spToneMap.Get());
	hr = m_spToneMap->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE, D2D1_HDRTONEMAP_DISPLAY_MODE_SDR);
	if (FAILED(hr))
		return hr;
	// FP16 output is still scene-referred: no additional white scaling here.
	hr = m_spWhiteAdjust->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, 80.0f);
	if (SUCCEEDED(hr))
		hr = m_spWhiteAdjust->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, 80.0f);
	if (FAILED(hr))
		return hr;
	m_spDevice = pDevice;
	return S_OK;
}

HRESULT KHdrToneMapper::resize(UINT nWidth, UINT nHeight)
{
	m_spContext->SetTarget(nullptr);
	m_spToneMap->SetInput(0, nullptr);
	m_spInputBitmap.Reset();
	m_spOutputBitmap.Reset();
	m_spInput.Reset();
	m_spOutput.Reset();
	m_nWidth = m_nHeight = 0;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = nWidth;
	desc.Height = nHeight;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	HRESULT hr = m_spDevice->CreateTexture2D(&desc, nullptr, &m_spInput);
	if (SUCCEEDED(hr))
		hr = m_spDevice->CreateTexture2D(&desc, nullptr, &m_spOutput);
	if (FAILED(hr))
		return hr;
	ComPtr<IDXGISurface> spSurface;
	hr = m_spInput.As(&spSurface);
	if (FAILED(hr))
		return hr;
	auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
		D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE));
	hr = m_spContext->CreateBitmapFromDxgiSurface(spSurface.Get(), &properties, &m_spInputBitmap);
	if (FAILED(hr))
		return hr;
	spSurface.Reset();
	hr = m_spOutput.As(&spSurface);
	if (FAILED(hr))
		return hr;
	properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
	hr = m_spContext->CreateBitmapFromDxgiSurface(spSurface.Get(), &properties, &m_spOutputBitmap);
	if (FAILED(hr))
		return hr;
	m_spToneMap->SetInput(0, m_spInputBitmap.Get());
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	return S_OK;
}

HRESULT KHdrToneMapper::convert(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
	ID3D11Texture2D *pSource, float fWhiteScale, float fReferencePeakNits, ID3D11Texture2D **ppOutput)
{
	if (!ppOutput)
		return E_POINTER;
	*ppOutput = nullptr;
	if (!pDevice || !pContext || !pSource || !std::isfinite(fWhiteScale) || fWhiteScale <= 0
		|| !std::isfinite(fReferencePeakNits) || fReferencePeakNits < fWhiteScale * 80.0f)
		return E_INVALIDARG;
	ComPtr<ID3D11Device> spOwner;
	pSource->GetDevice(&spOwner);
	if (spOwner.Get() != pDevice)
		return E_INVALIDARG;
	pContext->GetDevice(spOwner.ReleaseAndGetAddressOf());
	if (spOwner.Get() != pDevice)
		return E_INVALIDARG;
	D3D11_TEXTURE2D_DESC desc = {};
	pSource->GetDesc(&desc);
	if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.SampleDesc.Count != 1
		|| desc.ArraySize != 1 || desc.MipLevels != 1 || !desc.Width || !desc.Height)
		return E_INVALIDARG;
	HRESULT hr = pDevice->GetDeviceRemovedReason();
	if (FAILED(hr))
		return hr;
	if (m_spDevice.Get() != pDevice)
	{
		hr = initialize(pDevice);
		if (FAILED(hr))
			return hr;
	}
	if (m_nWidth != desc.Width || m_nHeight != desc.Height)
	{
		hr = resize(desc.Width, desc.Height);
		if (FAILED(hr))
			return hr;
	}
	hr = m_spToneMap->SetValue(D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE, fReferencePeakNits);
	if (SUCCEEDED(hr))
		hr = m_spToneMap->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, fWhiteScale * 80.0f);
	if (FAILED(hr))
		return hr;
	pContext->CopyResource(m_spInput.Get(), pSource);
	m_spContext->SetTarget(m_spOutputBitmap.Get());
	m_spContext->BeginDraw();
	ComPtr<ID2D1Image> spImage;
	m_spWhiteAdjust->GetOutput(&spImage);
	m_spContext->DrawImage(spImage.Get(), nullptr, nullptr,
		D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
	hr = m_spContext->EndDraw();
	m_spContext->SetTarget(nullptr);
	if (FAILED(hr))
		return hr;
	hr = pDevice->GetDeviceRemovedReason();
	return FAILED(hr) ? hr : m_spOutput.CopyTo(ppOutput);
}
