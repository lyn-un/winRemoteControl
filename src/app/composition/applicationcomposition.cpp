#include "app/composition/applicationcomposition.h"

#include "adapters/windows/device/windowsdeviceinfoprovider.h"
#include "adapters/windows/input/windowsinputinjector.h"
#include "capture/captureservice.h"
#include "session/sessionviewmodel.h"
#include "transport/webrtc/webrtcpeer.h"
#include "transport/webrtc/webrtcsessionservice.h"

#include <memory>

KApplicationComposition::KApplicationComposition(QObject *pParent)
	: QObject(pParent)
	, m_pCaptureService(new KCaptureService(this))
	, m_pSessionService(new KWebRtcSessionService(
		std::make_unique<KWindowsDeviceInfoProvider>(),
		std::make_unique<KWindowsInputInjector>(),
		std::make_unique<KWebRtcPeer>(),
		this))
	, m_pSessionViewModel(new KSessionViewModel(
		m_pCaptureService,
		m_pSessionService,
		this))
{
	wireServices();
}

KApplicationComposition::~KApplicationComposition()
{
	shutdown();
}

KSessionViewModel *KApplicationComposition::sessionViewModel() const
{
	return m_pSessionViewModel;
}

void KApplicationComposition::shutdown()
{
	if (m_bShutdown)
		return;

	m_bShutdown = true;
	m_pSessionService->disconnectSession();
	m_pCaptureService->stopCapture();
}

void KApplicationComposition::wireServices()
{
	connect(m_pCaptureService, &KCaptureService::webRtcFrameReady,
		m_pSessionService, &KWebRtcSessionService::pushVideoFrame);
	connect(m_pCaptureService, &KCaptureService::captureError,
		m_pSessionService, &KWebRtcSessionService::handleCaptureFailure);

	connect(m_pSessionService, &KWebRtcSessionService::startCaptureRequested,
		m_pCaptureService, &KCaptureService::startWebRtcCapture);
	connect(m_pSessionService, &KWebRtcSessionService::stopCaptureRequested,
		m_pCaptureService, &KCaptureService::stopCapture);
	connect(m_pSessionService, &KWebRtcSessionService::streamConfigChanged,
		m_pCaptureService, &KCaptureService::setStreamConfig);
	connect(m_pSessionService, &KWebRtcSessionService::inputTraceUpdated,
		m_pCaptureService, &KCaptureService::setInputTraceState);
	connect(m_pSessionService, &KWebRtcSessionService::inputFeedbackFrameRequested,
		m_pCaptureService, &KCaptureService::requestImmediateFrame);
}
