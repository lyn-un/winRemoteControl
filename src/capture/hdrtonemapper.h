#ifndef _WINREMOTECONTROL_HDRTONEMAPPER_H_
#define _WINREMOTECONTROL_HDRTONEMAPPER_H_

#include <d3d11.h>
#include <d2d1_1.h>
#include <wrl/client.h>

// Offline candidate: output remains linear scRGB; the BGRA converter encodes sRGB once.
class KHdrToneMapper
{
public:
	KHdrToneMapper() = default;
	KHdrToneMapper(const KHdrToneMapper &) = delete;
	KHdrToneMapper &operator=(const KHdrToneMapper &) = delete;

	HRESULT convert(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
		ID3D11Texture2D *pSource, float fWhiteScale, float fReferencePeakNits,
		ID3D11Texture2D **ppOutput);
	void reset();

private:
	HRESULT initialize(ID3D11Device *pDevice);
	HRESULT resize(UINT nWidth, UINT nHeight);
	Microsoft::WRL::ComPtr<ID3D11Device> m_spDevice;
	Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_spContext;
	Microsoft::WRL::ComPtr<ID2D1Effect> m_spToneMap;
	Microsoft::WRL::ComPtr<ID2D1Effect> m_spWhiteAdjust;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spInput;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spOutput;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_spInputBitmap;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_spOutputBitmap;
	UINT m_nWidth = 0;
	UINT m_nHeight = 0;
};

#endif // _WINREMOTECONTROL_HDRTONEMAPPER_H_
