#ifndef _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_
#define _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_

#include "core/media/capturesource.h"
#include "core/media/hdrconversionmode.h"
#include "capture/hdrgpubgraconverter.h"
#include "capture/d3d11bgraframe.h"
#include "core/media/videoframepath.h"

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
	explicit KDxgiDesktopDuplicator(KHdrConversionMode mode = AutoHdrConversionMode,
		std::shared_ptr<KVideoFramePathState> spFramePath = nullptr);
	~KDxgiDesktopDuplicator();

	KDxgiDesktopDuplicator(const KDxgiDesktopDuplicator &) = delete;
	KDxgiDesktopDuplicator &operator=(const KDxgiDesktopDuplicator &) = delete;

	bool initialize(QString *pErrorMessage) override;
	void shutdown() override;
	CaptureResult captureNextFrame(KCaptureFrame *pFrame, QString *pErrorMessage) override;
	bool takeCursorUpdate(KCursorShape *pShape) override;

private:
	friend struct KHdrCaptureTestAccess;
	bool initializeDevice(QString *pErrorMessage);
	bool detectHdrOutput(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput, QString *pErrorMessage);
	bool createDuplication(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput, QString *pErrorMessage);
	CaptureResult releaseCapturedFrame(CaptureResult result, QString *pErrorMessage);
	bool createStagingTexture(const D3D11_TEXTURE2D_DESC &sourceDesc, QString *pErrorMessage);
	bool ensureLastDesktopTexture(const D3D11_TEXTURE2D_DESC &sourceDesc, QString *pErrorMessage);
	bool captureInitialFrameWithGdi(KCaptureFrame *pFrame, QString *pErrorMessage);
	bool updatePointerState(const DXGI_OUTDUPL_FRAME_INFO &frameInfo, QString *pErrorMessage);
	void queueCursorShape();
	bool composePointer(KCaptureFrame *pFrame, QString *pErrorMessage) const;
	CaptureResult capturePointerOnlyFrame(KCaptureFrame *pFrame, QString *pErrorMessage);
	bool copyTextureToBgraFrame(ID3D11Texture2D *pSourceTexture, KCaptureFrame *pFrame, QString *pErrorMessage);
	CaptureResult copyTextureToNativeFrame(ID3D11Texture2D *pSourceTexture,
		KCaptureFrame *pFrame, QString *pErrorMessage);
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
	KHdrGpuBgraConverter m_hdrConverter;
	KHdrConversionMode m_hdrMode = AutoHdrConversionMode;
	bool m_bHdrGpuFailed = false;
	DXGI_FORMAT m_lastLoggedFormat = DXGI_FORMAT_UNKNOWN;
	UINT m_nLastLoggedWidth = 0;
	UINT m_nLastLoggedHeight = 0;
	DXGI_OUTDUPL_POINTER_POSITION m_pointerPosition = {};
	DXGI_OUTDUPL_POINTER_SHAPE_INFO m_pointerShapeInfo = {};
	std::vector<unsigned char> m_vecPointerShapeBuffer;
	quint64 m_nPointerUpdateCount = 0;
	quint64 m_nPointerOnlyFrameCount = 0;
	KCursorShape m_lastCursorShape;
	KCursorShape m_pendingCursorShape;
	bool m_bHasPendingCursorShape = false;
	bool m_bComInitialized = false;
	std::shared_ptr<KVideoFramePathState> m_spFramePath;
	std::vector<std::shared_ptr<KD3D11BgraFrame>> m_nativeFrames;
	quint64 m_nAdapterLuid = 0;
	quint64 m_nNativePoolDrops = 0;
	quint64 m_nSourceGeneration = 0;
	quint64 m_nAcquiredFrames = 0;
	quint64 m_nReleasedFrames = 0;
	HRESULT m_hrLastRelease = S_OK;
};

#endif // _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_
