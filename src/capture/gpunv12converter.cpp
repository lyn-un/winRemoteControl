#include "capture/gpunv12converter.h"

#include <d3dcompiler.h>

#include <cstring>

namespace
{
// Input is gamma-encoded SDR RGB, not linear RGB. Sampling uses an UNORM SRV
// even when the producer wrote through an sRGB RTV (HDR conversion output).
constexpr char kShader[] = R"(
Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Parameters : register(b0) { float2 outputSize; float2 padding; };
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 ReadRgb(float2 pixel)
{
    return source.SampleLevel(linearClamp, pixel / outputSize, 0).rgb;
}
float PSY(float4 position : SV_Position) : SV_Target
{
    return (16 + dot(ReadRgb(position.xy), float3(65.481, 128.553, 24.966))) / 255;
}
float2 PSUV(float4 position : SV_Position) : SV_Target
{
    float2 pixel = floor(position.xy) * 2 + 0.5;
    float3 rgb = (ReadRgb(pixel) + ReadRgb(pixel + float2(1, 0))
        + ReadRgb(pixel + float2(0, 1)) + ReadRgb(pixel + float2(1, 1))) * 0.25;
    return float2(128 + dot(rgb, float3(-37.797, -74.203, 112)),
                  128 + dot(rgb, float3(112, -93.786, -18.214))) / 255;
}
)";

HRESULT CompileShader(const char *pEntry, const char *pTarget, ID3DBlob **ppCode)
{
	return D3DCompile(kShader, std::strlen(kShader), "gpu-nv12", nullptr, nullptr,
		pEntry, pTarget, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
		0, ppCode, nullptr);
}
}

void KGpuNv12Converter::reset()
{
	for (KOutputViews &views : m_outputViews)
		views = {};
	m_spInputView.Reset();
	m_spInput.Reset();
	m_spRasterizer.Reset();
	m_spSampler.Reset();
	m_spConstants.Reset();
	m_spUvShader.Reset();
	m_spYShader.Reset();
	m_spVertexShader.Reset();
	m_spDevice.Reset();
	m_nInputWidth = 0;
	m_nInputHeight = 0;
	m_nNextView = 0;
}

HRESULT KGpuNv12Converter::initialize(ID3D11Device *pDevice)
{
	reset();
	Microsoft::WRL::ComPtr<ID3DBlob> spCode;
	HRESULT hr = CompileShader("VS", "vs_5_0", &spCode);
	if (FAILED(hr))
		return hr;
	hr = pDevice->CreateVertexShader(spCode->GetBufferPointer(), spCode->GetBufferSize(),
		nullptr, &m_spVertexShader);
	if (FAILED(hr))
		return hr;
	spCode.Reset();
	hr = CompileShader("PSY", "ps_5_0", &spCode);
	if (FAILED(hr))
		return hr;
	hr = pDevice->CreatePixelShader(spCode->GetBufferPointer(), spCode->GetBufferSize(), nullptr, &m_spYShader);
	if (FAILED(hr))
		return hr;
	spCode.Reset();
	hr = CompileShader("PSUV", "ps_5_0", &spCode);
	if (FAILED(hr))
		return hr;
	hr = pDevice->CreatePixelShader(spCode->GetBufferPointer(), spCode->GetBufferSize(), nullptr, &m_spUvShader);
	if (FAILED(hr))
		return hr;
	D3D11_BUFFER_DESC buffer = {};
	buffer.ByteWidth = 16;
	buffer.Usage = D3D11_USAGE_DEFAULT;
	buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = pDevice->CreateBuffer(&buffer, nullptr, &m_spConstants);
	if (FAILED(hr))
		return hr;
	D3D11_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	hr = pDevice->CreateSamplerState(&sampler, &m_spSampler);
	if (FAILED(hr))
		return hr;
	D3D11_RASTERIZER_DESC raster = {};
	raster.FillMode = D3D11_FILL_SOLID;
	raster.CullMode = D3D11_CULL_NONE;
	raster.DepthClipEnable = TRUE;
	hr = pDevice->CreateRasterizerState(&raster, &m_spRasterizer);
	if (FAILED(hr))
		return hr;
	m_spDevice = pDevice;
	return S_OK;
}

HRESULT KGpuNv12Converter::resizeInput(UINT nWidth, UINT nHeight)
{
	m_spInputView.Reset();
	m_spInput.Reset();
	m_nInputWidth = 0;
	m_nInputHeight = 0;
	D3D11_TEXTURE2D_DESC descriptor = {};
	descriptor.Width = nWidth;
	descriptor.Height = nHeight;
	descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
	descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	descriptor.Usage = D3D11_USAGE_DEFAULT;
	descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = m_spDevice->CreateTexture2D(&descriptor, nullptr, &m_spInput);
	if (FAILED(hr))
		return hr;
	hr = m_spDevice->CreateShaderResourceView(m_spInput.Get(), nullptr, &m_spInputView);
	if (FAILED(hr))
		return hr;
	m_nInputWidth = nWidth;
	m_nInputHeight = nHeight;
	return S_OK;
}

HRESULT KGpuNv12Converter::outputViews(ID3D11Texture2D *pTexture, KOutputViews **ppViews)
{
	for (KOutputViews &views : m_outputViews)
	{
		if (views.pTexture == pTexture)
		{
			*ppViews = &views;
			return S_OK;
		}
	}
	KOutputViews &views = m_outputViews[m_nNextView];
	views = {};
	D3D11_RENDER_TARGET_VIEW_DESC descriptor = {};
	descriptor.Format = DXGI_FORMAT_R8_UNORM;
	descriptor.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	HRESULT hr = m_spDevice->CreateRenderTargetView(pTexture, &descriptor, &views.spY);
	if (FAILED(hr))
		return hr;
	descriptor.Format = DXGI_FORMAT_R8G8_UNORM;
	hr = m_spDevice->CreateRenderTargetView(pTexture, &descriptor, &views.spUv);
	if (FAILED(hr))
	{
		views = {};
		return hr;
	}
	views.pTexture = pTexture;
	m_nNextView = (m_nNextView + 1) % static_cast<UINT>(m_outputViews.size());
	*ppViews = &views;
	return S_OK;
}

HRESULT KGpuNv12Converter::convert(ID3D11DeviceContext *pContext, ID3D11Texture2D *pSource,
	ID3D11Texture2D *pDestination, UINT nWidth, UINT nHeight)
{
	if (!pContext || !pSource || !pDestination || nWidth < 2 || nHeight < 2 || (nWidth & 1) || (nHeight & 1))
		return E_INVALIDARG;
	Microsoft::WRL::ComPtr<ID3D11Device> spDevice;
	Microsoft::WRL::ComPtr<ID3D11Device> spResourceDevice;
	pContext->GetDevice(&spDevice);
	pSource->GetDevice(&spResourceDevice);
	if (spResourceDevice.Get() != spDevice.Get())
		return E_INVALIDARG;
	pDestination->GetDevice(spResourceDevice.ReleaseAndGetAddressOf());
	if (spResourceDevice.Get() != spDevice.Get())
		return E_INVALIDARG;
	D3D11_TEXTURE2D_DESC source = {}, destination = {};
	pSource->GetDesc(&source);
	pDestination->GetDesc(&destination);
	if ((source.Format != DXGI_FORMAT_B8G8R8A8_UNORM && source.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS
		&& source.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) || !source.Width || !source.Height
		|| source.ArraySize != 1 || source.MipLevels != 1 || source.SampleDesc.Count != 1
		|| destination.Format != DXGI_FORMAT_NV12 || destination.Width < nWidth || destination.Height < nHeight
		|| destination.ArraySize != 1 || destination.MipLevels != 1 || destination.SampleDesc.Count != 1
		|| !(destination.BindFlags & D3D11_BIND_RENDER_TARGET))
		return E_INVALIDARG;
	HRESULT hr = spDevice->GetDeviceRemovedReason();
	if (FAILED(hr))
		return hr;
	if (m_spDevice.Get() != spDevice.Get())
	{
		hr = initialize(spDevice.Get());
		if (FAILED(hr))
			return hr;
	}
	if (m_nInputWidth != source.Width || m_nInputHeight != source.Height)
	{
		hr = resizeInput(source.Width, source.Height);
		if (FAILED(hr))
			return hr;
	}
	KOutputViews *pViews = nullptr;
	hr = outputViews(pDestination, &pViews);
	if (FAILED(hr))
		return hr;
	const float parameters[4] = {static_cast<float>(nWidth), static_cast<float>(nHeight), 0, 0};
	pContext->UpdateSubresource(m_spConstants.Get(), 0, nullptr, parameters, 0, 0);
	pContext->CopyResource(m_spInput.Get(), pSource);
	// Initialize alignment padding as limited-range black, not undefined memory.
	const float black[4] = {16.0f / 255, 0, 0, 1};
	const float neutral[4] = {128.0f / 255, 128.0f / 255, 0, 1};
	pContext->ClearRenderTargetView(pViews->spY.Get(), black);
	pContext->ClearRenderTargetView(pViews->spUv.Get(), neutral);
	pContext->RSSetState(m_spRasterizer.Get());
	pContext->IASetInputLayout(nullptr);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pContext->VSSetShader(m_spVertexShader.Get(), nullptr, 0);
	pContext->GSSetShader(nullptr, nullptr, 0);
	pContext->HSSetShader(nullptr, nullptr, 0);
	pContext->DSSetShader(nullptr, nullptr, 0);
	pContext->PSSetShaderResources(0, 1, m_spInputView.GetAddressOf());
	pContext->PSSetSamplers(0, 1, m_spSampler.GetAddressOf());
	pContext->PSSetConstantBuffers(0, 1, m_spConstants.GetAddressOf());
	pContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	pContext->OMSetDepthStencilState(nullptr, 0);
	D3D11_VIEWPORT viewport = {};
	viewport.Width = static_cast<float>(nWidth);
	viewport.Height = static_cast<float>(nHeight);
	viewport.MaxDepth = 1;
	pContext->RSSetViewports(1, &viewport);
	pContext->PSSetShader(m_spYShader.Get(), nullptr, 0);
	pContext->OMSetRenderTargets(1, pViews->spY.GetAddressOf(), nullptr);
	pContext->Draw(3, 0);
	viewport.Width /= 2;
	viewport.Height /= 2;
	pContext->RSSetViewports(1, &viewport);
	pContext->PSSetShader(m_spUvShader.Get(), nullptr, 0);
	pContext->OMSetRenderTargets(1, pViews->spUv.GetAddressOf(), nullptr);
	pContext->Draw(3, 0);
	ID3D11ShaderResourceView *pNullView = nullptr;
	ID3D11SamplerState *pNullSampler = nullptr;
	ID3D11Buffer *pNullBuffer = nullptr;
	pContext->PSSetShaderResources(0, 1, &pNullView);
	pContext->PSSetSamplers(0, 1, &pNullSampler);
	pContext->PSSetConstantBuffers(0, 1, &pNullBuffer);
	pContext->OMSetRenderTargets(0, nullptr, nullptr);
	pContext->VSSetShader(nullptr, nullptr, 0);
	pContext->PSSetShader(nullptr, nullptr, 0);
	pContext->RSSetState(nullptr);
	return spDevice->GetDeviceRemovedReason();
}
