#ifndef _WINREMOTECONTROL_D3DI420PIPELINE_H_
#define _WINREMOTECONTROL_D3DI420PIPELINE_H_

#include "core/media/decodedvideoframe.h"
#include <d3d11.h>
#include <wrl/client.h>

// Windows-only texture/conversion resources shared by the widget and offscreen tests.
class KD3dI420Pipeline
{
public:
	HRESULT upload(ID3D11Device *pDevice, ID3D11DeviceContext *pContext, const KDecodedVideoFrame &frame);
	void bind(ID3D11DeviceContext *pContext) const;
	void clear();
	bool isReady() const
	{
		return m_views[0] && m_spShader;
	}

private:
	std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> m_textures;
	std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 3> m_views;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_spShader;
	int m_nWidth = 0;
	int m_nHeight = 0;
};

#endif // _WINREMOTECONTROL_D3DI420PIPELINE_H_
