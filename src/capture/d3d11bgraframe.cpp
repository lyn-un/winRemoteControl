#include "capture/d3d11bgraframe.h"

#include <cstring>

KD3D11BgraFrame::KD3D11BgraFrame(ID3D11Texture2D *pTexture,
	std::shared_ptr<std::recursive_mutex> spContextMutex, quint64 nGeneration, quint64 nAdapterLuid)
	: m_spTexture(pTexture)
	, m_spContextMutex(std::move(spContextMutex))
	, m_nGeneration(nGeneration)
	, m_nAdapterLuid(nAdapterLuid)
{
	D3D11_TEXTURE2D_DESC descriptor = {};
	if (!pTexture)
		return;
	pTexture->GetDesc(&descriptor);
	if (descriptor.Format != DXGI_FORMAT_B8G8R8A8_UNORM || descriptor.ArraySize != 1
		|| descriptor.MipLevels != 1 || descriptor.SampleDesc.Count != 1)
	{
		m_spTexture.Reset();
		return;
	}
	m_nWidth = static_cast<int>(descriptor.Width);
	m_nHeight = static_cast<int>(descriptor.Height);
}

HRESULT KD3D11BgraFrame::copyBgra(std::vector<unsigned char> *pPixels) const
{
	if (!pPixels || !m_spTexture || !m_spContextMutex)
		return E_INVALIDARG;
	std::lock_guard<std::recursive_mutex> guard(*m_spContextMutex);
	Microsoft::WRL::ComPtr<ID3D11Device> spDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> spContext;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spStaging;
	m_spTexture->GetDevice(&spDevice);
	spDevice->GetImmediateContext(&spContext);
	D3D11_TEXTURE2D_DESC descriptor = {};
	m_spTexture->GetDesc(&descriptor);
	descriptor.Usage = D3D11_USAGE_STAGING;
	descriptor.BindFlags = descriptor.MiscFlags = 0;
	descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	HRESULT hr = spDevice->CreateTexture2D(&descriptor, nullptr, &spStaging);
	if (FAILED(hr))
		return hr;
	spContext->CopyResource(spStaging.Get(), m_spTexture.Get());
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	hr = spContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr))
		return hr;
	pPixels->resize(static_cast<size_t>(m_nWidth) * m_nHeight * 4);
	for (int y = 0; y < m_nHeight; ++y)
		std::memcpy(pPixels->data() + static_cast<size_t>(y) * m_nWidth * 4,
			static_cast<const unsigned char *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
			static_cast<size_t>(m_nWidth) * 4);
	spContext->Unmap(spStaging.Get(), 0);
	return S_OK;
}
