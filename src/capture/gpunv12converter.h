#ifndef _WINREMOTECONTROL_GPUNV12CONVERTER_H_
#define _WINREMOTECONTROL_GPUNV12CONVERTER_H_

#include <d3d11.h>
#include <wrl/client.h>

#include <array>

// Caller serializes the immediate context and owns pipeline state. Output textures
// must remain reserved until the downstream encoder has released its frame.
class KGpuNv12Converter
{
public:
	KGpuNv12Converter() = default;
	KGpuNv12Converter(const KGpuNv12Converter &) = delete;
	KGpuNv12Converter &operator=(const KGpuNv12Converter &) = delete;

	HRESULT convert(ID3D11DeviceContext *pContext, ID3D11Texture2D *pSource,
		ID3D11Texture2D *pDestination, UINT nWidth, UINT nHeight);
	void reset();

private:
	struct KOutputViews
	{
		ID3D11Texture2D *pTexture = nullptr;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> spY;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> spUv;
	};
	HRESULT initialize(ID3D11Device *pDevice);
	HRESULT resizeInput(UINT nWidth, UINT nHeight);
	HRESULT outputViews(ID3D11Texture2D *pTexture, KOutputViews **ppViews);

	Microsoft::WRL::ComPtr<ID3D11Device> m_spDevice;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_spVertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_spYShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_spUvShader;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_spConstants;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_spSampler;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_spRasterizer;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spInput;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_spInputView;
	std::array<KOutputViews, 8> m_outputViews;
	UINT m_nNextView = 0;
	UINT m_nInputWidth = 0;
	UINT m_nInputHeight = 0;
};

#endif // _WINREMOTECONTROL_GPUNV12CONVERTER_H_
