#include "capture/hdrgpubgraconverter.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace
{
constexpr char kShader[] = R"(
Texture2D<float4> desktop : register(t0);
cbuffer Parameters : register(b0) { float whiteScale; float3 padding; };
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * float2(2, -2) + float2(-1, 1), 0, 1);
}
float CleanChannel(float value)
{
    // NaN has no meaningful luminance. Infinities are clipped by saturate.
    return isnan(value) ? 0.0f : saturate(value / whiteScale);
}
float4 PS(float4 position : SV_Position) : SV_Target
{
    float3 color = desktop.Load(int3(int2(position.xy), 0)).rgb;
    return float4(CleanChannel(color.r), CleanChannel(color.g), CleanChannel(color.b), 1);
}
)";

HRESULT CompileShader(const char *pEntry, const char *pTarget, ID3DBlob **ppCode)
{
#if defined(WRC_HDR_FAULT_TESTS)
	const HRESULT hrInjected = HdrCaptureTestFailure("shader");
	if (FAILED(hrInjected))
		return hrInjected;
#endif
	return D3DCompile(kShader, std::strlen(kShader), "hdr-bgra", nullptr, nullptr,
		pEntry, pTarget, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS
			| D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ppCode, nullptr);
}
}

void KHdrGpuBgraConverter::reset()
{
	m_spOutputView.Reset();
	m_spOutput.Reset();
	m_spInputView.Reset();
	m_spInput.Reset();
	m_spRasterizer.Reset();
	m_spConstants.Reset();
	m_spPixelShader.Reset();
	m_spVertexShader.Reset();
	m_spDevice.Reset();
	m_nWidth = 0;
	m_nHeight = 0;
	m_fWhiteScale = 0.0f;
}

HRESULT KHdrGpuBgraConverter::initialize(ID3D11Device *pDevice)
{
	reset();
	UINT nSupport = 0;
	HRESULT hr = pDevice->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &nSupport);
	if (FAILED(hr))
		return hr;
	if ((nSupport & (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD))
		!= (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD))
		return DXGI_ERROR_UNSUPPORTED;
	hr = pDevice->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, &nSupport);
	if (FAILED(hr))
		return hr;
	if (!(nSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
		return DXGI_ERROR_UNSUPPORTED;

	Microsoft::WRL::ComPtr<ID3DBlob> spCode;
	hr = CompileShader("VS", "vs_5_0", &spCode);
	if (FAILED(hr))
		return hr;
	hr = pDevice->CreateVertexShader(spCode->GetBufferPointer(), spCode->GetBufferSize(),
		nullptr, &m_spVertexShader);
	if (FAILED(hr))
		return hr;
	spCode.Reset();
	hr = CompileShader("PS", "ps_5_0", &spCode);
	if (FAILED(hr))
		return hr;
	hr = pDevice->CreatePixelShader(spCode->GetBufferPointer(), spCode->GetBufferSize(),
		nullptr, &m_spPixelShader);
	if (FAILED(hr))
		return hr;
	D3D11_BUFFER_DESC bufferDesc = {};
	bufferDesc.ByteWidth = 16;
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = pDevice->CreateBuffer(&bufferDesc, nullptr, &m_spConstants);
	if (FAILED(hr))
		return hr;
	D3D11_RASTERIZER_DESC rasterDesc = {};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = TRUE;
	hr = pDevice->CreateRasterizerState(&rasterDesc, &m_spRasterizer);
	if (FAILED(hr))
		return hr;
	m_spDevice = pDevice;
	return S_OK;
}

HRESULT KHdrGpuBgraConverter::resize(UINT nWidth, UINT nHeight)
{
	m_spOutputView.Reset();
	m_spOutput.Reset();
	m_spInputView.Reset();
	m_spInput.Reset();
	m_nWidth = 0;
	m_nHeight = 0;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = nWidth;
	desc.Height = nHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = m_spDevice->CreateTexture2D(&desc, nullptr, &m_spInput);
	if (FAILED(hr))
		return hr;
#if defined(WRC_HDR_FAULT_TESTS)
	hr = HdrCaptureTestFailure("srv");
	if (FAILED(hr))
		return hr;
#endif
	hr = m_spDevice->CreateShaderResourceView(m_spInput.Get(), nullptr, &m_spInputView);
	if (FAILED(hr))
		return hr;
	desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	hr = m_spDevice->CreateTexture2D(&desc, nullptr, &m_spOutput);
	if (FAILED(hr))
		return hr;
	D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
	viewDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
#if defined(WRC_HDR_FAULT_TESTS)
	hr = HdrCaptureTestFailure("rtv");
	if (FAILED(hr))
		return hr;
#endif
	hr = m_spDevice->CreateRenderTargetView(m_spOutput.Get(), &viewDesc, &m_spOutputView);
	if (FAILED(hr))
		return hr;
	m_nWidth = nWidth;
	m_nHeight = nHeight;
	return S_OK;
}

HRESULT KHdrGpuBgraConverter::convert(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
	ID3D11Texture2D *pSource, float fWhiteScale, ID3D11Texture2D **ppOutput)
{
	if (!ppOutput)
		return E_POINTER;
	*ppOutput = nullptr;
	if (!pDevice || !pContext || !pSource || !std::isfinite(fWhiteScale) || fWhiteScale <= 0)
		return E_INVALIDARG;
	Microsoft::WRL::ComPtr<ID3D11Device> spSourceDevice;
	pSource->GetDevice(&spSourceDevice);
	if (spSourceDevice.Get() != pDevice)
		return E_INVALIDARG;
	pContext->GetDevice(spSourceDevice.ReleaseAndGetAddressOf());
	if (spSourceDevice.Get() != pDevice)
		return E_INVALIDARG;
	D3D11_TEXTURE2D_DESC desc = {};
	pSource->GetDesc(&desc);
	if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.SampleDesc.Count != 1
		|| desc.MipLevels != 1 || desc.ArraySize != 1 || !desc.Width || !desc.Height)
		return E_INVALIDARG;
#if defined(WRC_HDR_FAULT_TESTS)
	const HRESULT hrInjected = HdrCaptureTestFailure("device");
	if (FAILED(hrInjected))
		return hrInjected;
#endif
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
	if (m_fWhiteScale != fWhiteScale)
	{
		const float parameters[4] = { fWhiteScale, 0, 0, 0 };
		pContext->UpdateSubresource(m_spConstants.Get(), 0, nullptr, parameters, 0, 0);
		m_fWhiteScale = fWhiteScale;
	}
	// Always copy to a reusable SRV texture: duplication textures need not permit SRVs.
	pContext->CopyResource(m_spInput.Get(), pSource);
	D3D11_VIEWPORT viewport = {};
	viewport.Width = static_cast<float>(desc.Width);
	viewport.Height = static_cast<float>(desc.Height);
	viewport.MaxDepth = 1.0f;
	pContext->RSSetViewports(1, &viewport);
	pContext->RSSetState(m_spRasterizer.Get());
	pContext->IASetInputLayout(nullptr);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pContext->VSSetShader(m_spVertexShader.Get(), nullptr, 0);
	pContext->GSSetShader(nullptr, nullptr, 0);
	pContext->HSSetShader(nullptr, nullptr, 0);
	pContext->DSSetShader(nullptr, nullptr, 0);
	pContext->PSSetShader(m_spPixelShader.Get(), nullptr, 0);
	pContext->PSSetConstantBuffers(0, 1, m_spConstants.GetAddressOf());
	pContext->PSSetShaderResources(0, 1, m_spInputView.GetAddressOf());
	pContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	pContext->OMSetDepthStencilState(nullptr, 0);
	pContext->OMSetRenderTargets(1, m_spOutputView.GetAddressOf(), nullptr);
	pContext->Draw(3, 0);
	ID3D11ShaderResourceView *pNullView = nullptr;
	ID3D11Buffer *pNullBuffer = nullptr;
	pContext->PSSetShaderResources(0, 1, &pNullView);
	pContext->PSSetConstantBuffers(0, 1, &pNullBuffer);
	pContext->OMSetRenderTargets(0, nullptr, nullptr);
	pContext->VSSetShader(nullptr, nullptr, 0);
	pContext->PSSetShader(nullptr, nullptr, 0);
	pContext->RSSetState(nullptr);
	hr = pDevice->GetDeviceRemovedReason();
	if (FAILED(hr))
		return hr;
	return m_spOutput.CopyTo(ppOutput);
}
