#ifndef _WINREMOTECONTROL_VIDEORENDERWIDGET_H_
#define _WINREMOTECONTROL_VIDEORENDERWIDGET_H_

#include "codec/decodedvideoframe.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPoint>
#include <QtCore/QRectF>
#include <QtWidgets/QWidget>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <mutex>

class KVideoRenderWidget : public QWidget
{
	Q_OBJECT

public:
	explicit KVideoRenderWidget(QWidget *pParent = nullptr);
	~KVideoRenderWidget() override;

	KVideoRenderWidget(const KVideoRenderWidget &) = delete;
	KVideoRenderWidget &operator=(const KVideoRenderWidget &) = delete;

public slots:
	void setRemoteScreenSize(int nWidth, int nHeight);
	void enqueueFrame(const KDecodedVideoFrame &frame);
	void presentFrame(const KDecodedVideoFrame &frame);
	void clearFrame();

signals:
	void renderError(const QString &strMessage);
	void remoteMouseMoveRequested(int nX, int nY);
	void remoteMouseButtonRequested(int nX, int nY, int nButton, bool bPressed);
	void remoteMouseWheelRequested(int nX, int nY, int nDelta);

protected:
	void resizeEvent(QResizeEvent *pEvent) override;
	void showEvent(QShowEvent *pEvent) override;
	void mouseMoveEvent(QMouseEvent *pEvent) override;
	void mousePressEvent(QMouseEvent *pEvent) override;
	void mouseReleaseEvent(QMouseEvent *pEvent) override;
	void wheelEvent(QWheelEvent *pEvent) override;
	QPaintEngine *paintEngine() const override;

private slots:
	void presentLatestFrame();

private:
	struct KVertex
	{
		float fPositionX = 0.0f;
		float fPositionY = 0.0f;
		float fTexcoordX = 0.0f;
		float fTexcoordY = 0.0f;
	};

	bool initializeD3d(QString *pErrorMessage);
	bool createRenderTarget(QString *pErrorMessage);
	bool createShaders(QString *pErrorMessage);
	bool createSampler(QString *pErrorMessage);
	bool ensureFrameTexture(int nWidth, int nHeight, QString *pErrorMessage);
	bool updateVertexBuffer(int nFrameWidth, int nFrameHeight, QString *pErrorMessage);
	void releaseRenderTarget();
	void releaseAll();
	void render();
	bool mapToRemotePoint(const QPointF &localPoint, QPoint *pRemotePoint) const;
	static int qtMouseButtonToRemoteButton(Qt::MouseButton button);
	static QString hresultMessage(const QString &strPrefix, HRESULT hr);

	std::mutex m_frameMutex;
	KDecodedVideoFrame m_latestFrame;
	QElapsedTimer m_mouseMoveThrottleTimer;
	bool m_bHasPendingFrame = false;
	bool m_bPresentQueued = false;
	bool m_bInitialized = false;
	int m_nFrameWidth = 0;
	int m_nFrameHeight = 0;
	int m_nRemoteScreenWidth = 0;
	int m_nRemoteScreenHeight = 0;
	QRectF m_frameDisplayRect;
	Microsoft::WRL::ComPtr<ID3D11Device> m_spDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_spContext;
	Microsoft::WRL::ComPtr<IDXGISwapChain> m_spSwapChain;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_spRenderTargetView;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_spFrameTexture;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_spFrameShaderResourceView;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_spSamplerState;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_spVertexShader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_spPixelShader;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> m_spInputLayout;
	Microsoft::WRL::ComPtr<ID3D11Buffer> m_spVertexBuffer;
};

#endif // _WINREMOTECONTROL_VIDEORENDERWIDGET_H_
