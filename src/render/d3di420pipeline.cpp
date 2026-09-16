#include "render/d3di420pipeline.h"

#include <d3dcompiler.h>
#include <cstring>

namespace
{
// Reconstruct RGB before bilinear filtering to match the existing BGRA sampler.
constexpr char kShader[] = R"(
Texture2D<float> planeY : register(t0);
Texture2D<float> planeU : register(t1);
Texture2D<float> planeV : register(t2);
float3 rgb(int2 p, int2 size) {
    p = clamp(p, int2(0,0), size - 1);
    float y = planeY.Load(int3(p,0)) * 255.0;
    float u = planeU.Load(int3(p/2,0)) * 255.0 - 128.0;
    float v = planeV.Load(int3(p/2,0)) * 255.0 - 128.0;
    float l = (y - 16.0) * 1.16438356;
    return saturate(float3(l + 1.59602678*v,
        l - 0.39176229*u - 0.81296764*v, l + 2.01723214*u) / 255.0);
}
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    uint w,h; planeY.GetDimensions(w,h);
    float2 p = uv * float2(w,h) - 0.5;
    int2 base = int2(floor(p)); float2 f = frac(p);
    float3 top = lerp(rgb(base,int2(w,h)),rgb(base+int2(1,0),int2(w,h)),f.x);
    float3 bottom = lerp(rgb(base+int2(0,1),int2(w,h)),rgb(base+int2(1,1),int2(w,h)),f.x);
    return float4(lerp(top,bottom,f.y),1.0);
})";
} // namespace

HRESULT KD3dI420Pipeline::upload(ID3D11Device *pDevice, ID3D11DeviceContext *pContext, const KDecodedVideoFrame &frame)
{
	if (!pDevice || !pContext || !frame.hasPixels() || frame.pixelFormat != KDecodedPixelFormat::I420)
		return E_INVALIDARG;
	if (!m_spShader)
	{
		Microsoft::WRL::ComPtr<ID3DBlob> spBlob;
		HRESULT hr = D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "main", "ps_4_0",
								D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &spBlob, nullptr);
		if (FAILED(hr))
			return hr;
		hr = pDevice->CreatePixelShader(spBlob->GetBufferPointer(), spBlob->GetBufferSize(), nullptr, &m_spShader);
		if (FAILED(hr))
			return hr;
	}
	ID3D11ShaderResourceView *emptyViews[3] = {};
	pContext->PSSetShaderResources(0, 3, emptyViews);
	if (m_nWidth != frame.nWidth || m_nHeight != frame.nHeight || !m_views[0])
	{
		for (size_t nPlane = 0; nPlane < 3; ++nPlane)
		{
			m_views[nPlane].Reset();
			m_textures[nPlane].Reset();
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = frame.planes[nPlane].nWidth;
			desc.Height = frame.planes[nPlane].nHeight;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &m_textures[nPlane]);
			if (SUCCEEDED(hr))
				hr = pDevice->CreateShaderResourceView(m_textures[nPlane].Get(), nullptr, &m_views[nPlane]);
			if (FAILED(hr))
			{
				clear();
				return hr;
			}
		}
		m_nWidth = frame.nWidth;
		m_nHeight = frame.nHeight;
	}
	for (size_t nPlane = 0; nPlane < 3; ++nPlane)
		pContext->UpdateSubresource(m_textures[nPlane].Get(), 0, nullptr, frame.planes[nPlane].pData,
									frame.planes[nPlane].nStride, 0);
	return S_OK;
}

void KD3dI420Pipeline::bind(ID3D11DeviceContext *pContext) const
{
	ID3D11ShaderResourceView *views[3] = {m_views[0].Get(), m_views[1].Get(), m_views[2].Get()};
	pContext->PSSetShader(m_spShader.Get(), nullptr, 0);
	pContext->PSSetShaderResources(0, 3, views);
}

void KD3dI420Pipeline::clear()
{
	for (auto &view : m_views)
		view.Reset();
	for (auto &texture : m_textures)
		texture.Reset();
	m_spShader.Reset();
	m_nWidth = 0;
	m_nHeight = 0;
}
