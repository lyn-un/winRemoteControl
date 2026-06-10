#ifndef _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_
#define _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_

#include "capture/captureframe.h"

#include <QtCore/QString>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

class KDxgiDesktopDuplicator
{
public:
	enum CaptureResult
	{
		CapturedCaptureResult,
		TimeoutCaptureResult,
		ErrorCaptureResult
	};

	KDxgiDesktopDuplicator();
	~KDxgiDesktopDuplicator();

	KDxgiDesktopDuplicator(const KDxgiDesktopDuplicator &) = delete;
	KDxgiDesktopDuplicator &operator=(const KDxgiDesktopDuplicator &) = delete;

	bool initialize(QString *pErrorMessage);
	void shutdown();
	CaptureResult captureNextFrame(KCaptureFrame *pFrame, QString *pErrorMessage);

private:
	bool detectHdrOutput(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput, QString *pErrorMessage);
	bool createDuplication(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput, QString *pErrorMessage);
	bool createStagingTexture(const D3D11_TEXTURE2D_DESC &sourceDesc, QString *pErrorMessage);
	static QString hresultMessage(const QString &strPrefix, HRESULT hr);

	Microsoft::WRL::ComPtr<ID3D11Device> m_spDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_spContext;
	Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_spDuplication;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spStagingTexture;
	D3D11_TEXTURE2D_DESC m_stagingDesc = {};
	quint64 m_nFrameIndex = 0;
	bool m_bHdrOutput = false;
	DXGI_FORMAT m_captureFormat = DXGI_FORMAT_UNKNOWN;
	float m_fSdrWhiteScale = 2.5f;
};

#endif // _WINREMOTECONTROL_DXGIDESKTOPDUPLICATOR_H_
