#ifndef _WINREMOTECONTROL_HDRGPUBGRACONVERTER_H_
#define _WINREMOTECONTROL_HDRGPUBGRACONVERTER_H_

#include <d3d11.h>
#include <wrl/client.h>

#if defined(WRC_HDR_FAULT_TESTS)
// Defined only by the separately compiled fault-test executable.
HRESULT HdrCaptureTestFailure(const char *pStage);
#endif

// The caller owns synchronization and pipeline state of the capture context.
class KHdrGpuBgraConverter
{
public:
	KHdrGpuBgraConverter() = default;
	KHdrGpuBgraConverter(const KHdrGpuBgraConverter &) = delete;
	KHdrGpuBgraConverter &operator=(const KHdrGpuBgraConverter &) = delete;

	HRESULT convert(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
		ID3D11Texture2D *pSource, float fWhiteScale, ID3D11Texture2D **ppOutput);
	void reset();

private:
	HRESULT initialize(ID3D11Device *pDevice);
	HRESULT resize(UINT nWidth, UINT nHeight);

	Microsoft::WRL::ComPtr<ID3D11Device> m_spDevice;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_spVertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_spPixelShader;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_spConstants;
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_spRasterizer;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spInput;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_spInputView;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spOutput;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_spOutputView;
	UINT m_nWidth = 0;
	UINT m_nHeight = 0;
	float m_fWhiteScale = 0.0f;
};

#endif // _WINREMOTECONTROL_HDRGPUBGRACONVERTER_H_
