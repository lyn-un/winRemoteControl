#ifndef _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_
#define _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_

#include "core/media/capturesource.h"

#include <QtCore/QString>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <vector>

class KDxgiDesktopDuplicator : public IKCaptureSource
{
public:
	KDxgiDesktopDuplicator();
	~KDxgiDesktopDuplicator();

	KDxgiDesktopDuplicator(const KDxgiDesktopDuplicator &) = delete;
	KDxgiDesktopDuplicator &operator=(const KDxgiDesktopDuplicator &) = delete;

	bool initialize(QString *pErrorMessage) override;
	void shutdown() override;
	CaptureResult captureNextFrame(KCaptureFrame *pFrame, QString *pErrorMessage) override;

private:
	bool detectHdrOutput(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput, QString *pErrorMessage);
	bool createDuplication(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput, QString *pErrorMessage);
	bool createStagingTexture(const D3D11_TEXTURE2D_DESC &sourceDesc, QString *pErrorMessage);
	bool ensureLastDesktopTexture(const D3D11_TEXTURE2D_DESC &sourceDesc, QString *pErrorMessage);
	bool captureInitialFrameWithGdi(KCaptureFrame *pFrame, QString *pErrorMessage);
	bool updatePointerState(const DXGI_OUTDUPL_FRAME_INFO &frameInfo, QString *pErrorMessage);
	bool composePointer(KCaptureFrame *pFrame, QString *pErrorMessage) const;
	CaptureResult capturePointerOnlyFrame(KCaptureFrame *pFrame, QString *pErrorMessage);
	bool copyTextureToBgraFrame(ID3D11Texture2D *pSourceTexture, KCaptureFrame *pFrame, QString *pErrorMessage);
	static QString hresultMessage(const QString &strPrefix, HRESULT hr);

	Microsoft::WRL::ComPtr<ID3D11Device> m_spDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_spContext;
	Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_spDuplication;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spStagingTexture;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spLastDesktopTexture;
	D3D11_TEXTURE2D_DESC m_stagingDesc = {};
	quint64 m_nFrameIndex = 0;
	bool m_bHdrOutput = false;
	DXGI_FORMAT m_captureFormat = DXGI_FORMAT_UNKNOWN;
	RECT m_outputRect = {};
	float m_fSdrWhiteScale = 2.5f;
	DXGI_OUTDUPL_POINTER_POSITION m_pointerPosition = {};
	DXGI_OUTDUPL_POINTER_SHAPE_INFO m_pointerShapeInfo = {};
	std::vector<unsigned char> m_vecPointerShapeBuffer;
	quint64 m_nPointerUpdateCount = 0;
	quint64 m_nPointerOnlyFrameCount = 0;
};

#endif // _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_
