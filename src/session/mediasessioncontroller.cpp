#include "session/mediasessioncontroller.h"

#include <algorithm>

KMediaSessionController::KMediaSessionController(QObject *pParent)
	: QObject(pParent)
{
}

void KMediaSessionController::setCapabilities(
	const KNegotiatedCapabilities &capabilities)
{
	m_capabilities = capabilities;
}

KStreamConfig KMediaSessionController::constrainedConfig(
	const KStreamConfig &config) const
{
	KStreamConfig result = config;
	if (!m_capabilities.bValid)
		return result;
	if (result.nWidth > 0)
		result.nWidth = std::min(result.nWidth, m_capabilities.nMaximumWidth);
	if (result.nHeight > 0)
		result.nHeight = std::min(result.nHeight, m_capabilities.nMaximumHeight);
	result.nFps = std::min(result.nFps, m_capabilities.nMaximumFps);
	result.nBitrateKbps = std::min(result.nBitrateKbps,
		m_capabilities.nMaximumBitrateKbps);
	return result;
}

void KMediaSessionController::startCapture(quint64 nGeneration)
{
	if (m_bCaptureActive)
		return;
	m_bCaptureActive = true;
	emit startCaptureRequested(nGeneration);
}

void KMediaSessionController::stopCapture(quint64 nGeneration)
{
	if (!m_bCaptureActive)
		return;
	m_bCaptureActive = false;
	emit stopCaptureRequested(nGeneration);
}

void KMediaSessionController::reset()
{
	m_capabilities = KNegotiatedCapabilities();
	m_bCaptureActive = false;
}

bool KMediaSessionController::isCaptureActive() const
{
	return m_bCaptureActive;
}
