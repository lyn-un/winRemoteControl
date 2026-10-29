#ifndef _WINREMOTECONTROL_D3D11BGRAFRAME_H_
#define _WINREMOTECONTROL_D3D11BGRAFRAME_H_

#include "core/media/nativevideoframe.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <vector>

class KD3D11BgraFrame final : public IKNativeVideoFrame
{
public:
	KD3D11BgraFrame(ID3D11Texture2D *pTexture,
		std::shared_ptr<std::recursive_mutex> spContextMutex, quint64 nGeneration, quint64 nAdapterLuid);
	NativeType nativeType() const override { return D3D11BgraNativeType; }
	int width() const override { return m_nWidth; }
	int height() const override { return m_nHeight; }
	quint64 generation() const override { return m_nGeneration; }
	quint64 adapterLuid() const override { return m_nAdapterLuid; }
	ID3D11Texture2D *texture() const { return m_spTexture.Get(); }
	std::shared_ptr<std::recursive_mutex> contextMutex() const { return m_spContextMutex; }
	HRESULT copyBgra(std::vector<unsigned char> *pPixels) const;

private:
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spTexture;
	std::shared_ptr<std::recursive_mutex> m_spContextMutex;
	int m_nWidth = 0;
	int m_nHeight = 0;
	quint64 m_nGeneration = 0;
	quint64 m_nAdapterLuid = 0;
};

#endif // _WINREMOTECONTROL_D3D11BGRAFRAME_H_
