#include "capture/dxgidesktopduplicator.h"

#include "common/latencytracelogger.h"

#include <QtCore/QDateTime>
#include <QtCore/QDebug>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>

namespace
{
float HalfToFloat(unsigned short nValue)
{
	const unsigned int nSign = (nValue & 0x8000) << 16;
	unsigned int nExponent = (nValue >> 10) & 0x1f;
	unsigned int nMantissa = nValue & 0x03ff;

	unsigned int nResult = 0;
	if (nExponent == 0)
	{
		if (nMantissa == 0)
		{
			nResult = nSign;
		}
		else
		{
			nExponent = 1;
			while ((nMantissa & 0x0400) == 0)
			{
				nMantissa <<= 1;
				--nExponent;
			}
			nMantissa &= 0x03ff;
			nResult = nSign | ((nExponent + 127 - 15) << 23) | (nMantissa << 13);
		}
	}
	else if (nExponent == 31)
	{
		nResult = nSign | 0x7f800000 | (nMantissa << 13);
	}
	else
	{
		nResult = nSign | ((nExponent + 127 - 15) << 23) | (nMantissa << 13);
	}

	float fValue = 0.0f;
	std::memcpy(&fValue, &nResult, sizeof(fValue));
	return fValue;
}

float LinearToSrgb(float fValue)
{
	fValue = std::clamp(fValue, 0.0f, 1.0f);
	if (fValue <= 0.0031308f)
		return fValue * 12.92f;

	return 1.055f * std::pow(fValue, 1.0f / 2.4f) - 0.055f;
}

unsigned char FloatToByte(float fValue)
{
	fValue = std::clamp(fValue, 0.0f, 1.0f);
	return static_cast<unsigned char>(fValue * 255.0f + 0.5f);
}

unsigned char UnormToByte(unsigned int nValue, unsigned int nMaxValue)
{
	return static_cast<unsigned char>((nValue * 255u + nMaxValue / 2u) / nMaxValue);
}

unsigned char MaskBit(const unsigned char *pRow, int x)
{
	return static_cast<unsigned char>((pRow[x / 8] >> (7 - x % 8)) & 1);
}

bool CopyMappedFrameToBgra(const D3D11_TEXTURE2D_DESC &sourceDesc,
	const D3D11_MAPPED_SUBRESOURCE &mappedResource,
	KCaptureFrame *pFrame,
	float fSdrWhiteScale)
{
	const int nWidth = static_cast<int>(sourceDesc.Width);
	const int nHeight = static_cast<int>(sourceDesc.Height);
	pFrame->vecBgraBuffer.resize(static_cast<size_t>(nWidth) * static_cast<size_t>(nHeight) * 4);

	const unsigned char *pSource = static_cast<const unsigned char *>(mappedResource.pData);
	unsigned char *pTarget = pFrame->vecBgraBuffer.data();
	const size_t nTargetRowBytes = static_cast<size_t>(nWidth) * 4;

	if (sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
		|| sourceDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
	{
		for (int y = 0; y < nHeight; ++y)
		{
			std::memcpy(pTarget + static_cast<size_t>(y) * nTargetRowBytes,
				pSource + static_cast<size_t>(y) * mappedResource.RowPitch,
				nTargetRowBytes);
			unsigned char *pTargetRow = pTarget + static_cast<size_t>(y) * nTargetRowBytes;
			for (int x = 0; x < nWidth; ++x)
				pTargetRow[x * 4 + 3] = 255;
		}
		return true;
	}

	if (sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
		|| sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
	{
		for (int y = 0; y < nHeight; ++y)
		{
			const unsigned char *pSourceRow = pSource + static_cast<size_t>(y) * mappedResource.RowPitch;
			unsigned char *pTargetRow = pTarget + static_cast<size_t>(y) * nTargetRowBytes;
			for (int x = 0; x < nWidth; ++x)
			{
				pTargetRow[x * 4 + 0] = pSourceRow[x * 4 + 2];
				pTargetRow[x * 4 + 1] = pSourceRow[x * 4 + 1];
				pTargetRow[x * 4 + 2] = pSourceRow[x * 4 + 0];
				pTargetRow[x * 4 + 3] = 255;
			}
		}
		return true;
	}

	if (sourceDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
	{
		for (int y = 0; y < nHeight; ++y)
		{
			const unsigned char *pSourceRow = pSource + static_cast<size_t>(y) * mappedResource.RowPitch;
			unsigned char *pTargetRow = pTarget + static_cast<size_t>(y) * nTargetRowBytes;
			const unsigned int *pSourcePixel = reinterpret_cast<const unsigned int *>(pSourceRow);
			for (int x = 0; x < nWidth; ++x)
			{
				const unsigned int nPixel = pSourcePixel[x];
				const unsigned int nRed = nPixel & 0x3ff;
				const unsigned int nGreen = (nPixel >> 10) & 0x3ff;
				const unsigned int nBlue = (nPixel >> 20) & 0x3ff;
				pTargetRow[x * 4 + 0] = UnormToByte(nBlue, 1023);
				pTargetRow[x * 4 + 1] = UnormToByte(nGreen, 1023);
				pTargetRow[x * 4 + 2] = UnormToByte(nRed, 1023);
				pTargetRow[x * 4 + 3] = 255;
			}
		}
		return true;
	}

	if (sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		for (int y = 0; y < nHeight; ++y)
		{
			const unsigned char *pSourceRow = pSource + static_cast<size_t>(y) * mappedResource.RowPitch;
			unsigned char *pTargetRow = pTarget + static_cast<size_t>(y) * nTargetRowBytes;
			const std::uint16_t *pSourcePixel = reinterpret_cast<const std::uint16_t *>(pSourceRow);
			for (int x = 0; x < nWidth; ++x)
			{
				const std::uint16_t *pPixel = pSourcePixel + x * 4;
				float fRed = HalfToFloat(pPixel[0]);
				float fGreen = HalfToFloat(pPixel[1]);
				float fBlue = HalfToFloat(pPixel[2]);

				fRed = std::clamp(fRed / fSdrWhiteScale, 0.0f, 1.0f);
				fGreen = std::clamp(fGreen / fSdrWhiteScale, 0.0f, 1.0f);
				fBlue = std::clamp(fBlue / fSdrWhiteScale, 0.0f, 1.0f);

				pTargetRow[x * 4 + 0] = FloatToByte(LinearToSrgb(fBlue));
				pTargetRow[x * 4 + 1] = FloatToByte(LinearToSrgb(fGreen));
				pTargetRow[x * 4 + 2] = FloatToByte(LinearToSrgb(fRed));
				pTargetRow[x * 4 + 3] = 255;
			}
		}
		return true;
	}

	return false;
}
}

KDxgiDesktopDuplicator::KDxgiDesktopDuplicator()
{
}

KDxgiDesktopDuplicator::~KDxgiDesktopDuplicator()
{
	shutdown();
}

bool KDxgiDesktopDuplicator::initialize(QString *pErrorMessage)
{
	shutdown();

	const D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0
	};
	D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_11_0;

	UINT nFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
	nFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	HRESULT hr = ::D3D11CreateDevice(nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		nFlags,
		featureLevels,
		2,
		D3D11_SDK_VERSION,
		&m_spDevice,
		&selectedFeatureLevel,
		&m_spContext);
	if (FAILED(hr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("D3D11CreateDevice failed"), hr);
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIDevice> spDxgiDevice;
	hr = m_spDevice.As(&spDxgiDevice);
	if (FAILED(hr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Query IDXGIDevice failed"), hr);
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter> spAdapter;
	hr = spDxgiDevice->GetAdapter(&spAdapter);
	if (FAILED(hr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("GetAdapter failed"), hr);
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIOutput> spOutput;
	hr = spAdapter->EnumOutputs(0, &spOutput);
	if (FAILED(hr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("EnumOutputs(0) failed"), hr);
		return false;
	}

	if (!createDuplication(spOutput, pErrorMessage))
		return false;

	m_nFrameIndex = 0;
	return true;
}

void KDxgiDesktopDuplicator::shutdown()
{
	m_spStagingTexture.Reset();
	m_spDuplication.Reset();
	m_spContext.Reset();
	m_spDevice.Reset();
	m_stagingDesc = {};
	m_nFrameIndex = 0;
	m_bHdrOutput = false;
	m_captureFormat = DXGI_FORMAT_UNKNOWN;
	m_pointerPosition = {};
	m_pointerShapeInfo = {};
	m_vecPointerShapeBuffer.clear();
	m_nPointerUpdateCount = 0;
}

bool KDxgiDesktopDuplicator::detectHdrOutput(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput,
	QString *pErrorMessage)
{
	Microsoft::WRL::ComPtr<IDXGIOutput6> spOutput6;
	const HRESULT hrQuery = spOutput.As(&spOutput6);
	if (FAILED(hrQuery))
	{
		m_bHdrOutput = false;
		return true;
	}

	DXGI_OUTPUT_DESC1 outputDesc = {};
	const HRESULT hrDesc = spOutput6->GetDesc1(&outputDesc);
	if (FAILED(hrDesc))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("IDXGIOutput6::GetDesc1 failed"), hrDesc);
		return false;
	}

	m_bHdrOutput = outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
	qDebug() << "HDR output:" << m_bHdrOutput
		<< "BitsPerColor:" << outputDesc.BitsPerColor
		<< "ColorSpace:" << static_cast<unsigned int>(outputDesc.ColorSpace)
		<< "MaxLuminance:" << outputDesc.MaxLuminance
		<< "MinLuminance:" << outputDesc.MinLuminance;

	return true;
}

bool KDxgiDesktopDuplicator::createDuplication(const Microsoft::WRL::ComPtr<IDXGIOutput> &spOutput,
	QString *pErrorMessage)
{
	if (!detectHdrOutput(spOutput, pErrorMessage))
		return false;

	Microsoft::WRL::ComPtr<IDXGIOutput5> spOutput5;
	HRESULT hr = spOutput.As(&spOutput5);
	if (SUCCEEDED(hr))
	{
		DXGI_FORMAT supportedFormats[1] = {};
		supportedFormats[0] = m_bHdrOutput
			? DXGI_FORMAT_R16G16B16A16_FLOAT
			: DXGI_FORMAT_B8G8R8A8_UNORM;

		hr = spOutput5->DuplicateOutput1(m_spDevice.Get(),
			0,
			1,
			supportedFormats,
			&m_spDuplication);
		if (SUCCEEDED(hr))
		{
			DXGI_OUTDUPL_DESC duplicationDesc = {};
			m_spDuplication->GetDesc(&duplicationDesc);
			m_captureFormat = duplicationDesc.ModeDesc.Format;
			qDebug() << "Capture format:" << static_cast<unsigned int>(m_captureFormat);
			return true;
		}

		if (m_bHdrOutput)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = hresultMessage(QStringLiteral("DuplicateOutput1 failed"), hr);
			return false;
		}
	}
	else if (m_bHdrOutput)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Query IDXGIOutput5 failed"), hr);
		return false;
	}

	Microsoft::WRL::ComPtr<IDXGIOutput1> spOutput1;
	hr = spOutput.As(&spOutput1);
	if (FAILED(hr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Query IDXGIOutput1 failed"), hr);
		return false;
	}

	hr = spOutput1->DuplicateOutput(m_spDevice.Get(), &m_spDuplication);
	if (FAILED(hr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("DuplicateOutput failed"), hr);
		return false;
	}

	DXGI_OUTDUPL_DESC duplicationDesc = {};
	m_spDuplication->GetDesc(&duplicationDesc);
	m_captureFormat = duplicationDesc.ModeDesc.Format;
	qDebug() << "Capture format:" << static_cast<unsigned int>(m_captureFormat);
	return true;
}

KDxgiDesktopDuplicator::CaptureResult KDxgiDesktopDuplicator::captureNextFrame(KCaptureFrame *pFrame,
	QString *pErrorMessage)
{
	if (pFrame == nullptr)
		return ErrorCaptureResult;
	if (!m_spDuplication)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("DXGI duplication is not initialized");
		return ErrorCaptureResult;
	}

	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
	Microsoft::WRL::ComPtr<IDXGIResource> spDesktopResource;
	const HRESULT hrAcquire = m_spDuplication->AcquireNextFrame(100, &frameInfo, &spDesktopResource);
	if (hrAcquire == DXGI_ERROR_WAIT_TIMEOUT)
		return TimeoutCaptureResult;
	if (FAILED(hrAcquire))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("AcquireNextFrame failed"), hrAcquire);
		return ErrorCaptureResult;
	}
	if (!updatePointerState(frameInfo, pErrorMessage))
	{
		m_spDuplication->ReleaseFrame();
		return ErrorCaptureResult;
	}

	Microsoft::WRL::ComPtr<ID3D11Texture2D> spDesktopTexture;
	HRESULT hr = spDesktopResource.As(&spDesktopTexture);
	if (FAILED(hr))
	{
		m_spDuplication->ReleaseFrame();
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Query desktop texture failed"), hr);
		return ErrorCaptureResult;
	}

	D3D11_TEXTURE2D_DESC sourceDesc = {};
	spDesktopTexture->GetDesc(&sourceDesc);
	if (m_bHdrOutput && sourceDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		qWarning() << "HDR is enabled, but captured format is not FP16:"
			<< static_cast<unsigned int>(sourceDesc.Format);
	}

	if (!createStagingTexture(sourceDesc, pErrorMessage))
	{
		m_spDuplication->ReleaseFrame();
		return ErrorCaptureResult;
	}

	m_spContext->CopyResource(m_spStagingTexture.Get(), spDesktopTexture.Get());

	D3D11_MAPPED_SUBRESOURCE mappedResource = {};
	hr = m_spContext->Map(m_spStagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mappedResource);
	if (FAILED(hr))
	{
		m_spDuplication->ReleaseFrame();
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Map staging texture failed"), hr);
		return ErrorCaptureResult;
	}

	const int nWidth = static_cast<int>(sourceDesc.Width);
	const int nHeight = static_cast<int>(sourceDesc.Height);
	pFrame->nWidth = nWidth;
	pFrame->nHeight = nHeight;
	pFrame->nFrameIndex = ++m_nFrameIndex;
	pFrame->nTimestampMs = QDateTime::currentMSecsSinceEpoch();
	if (!CopyMappedFrameToBgra(sourceDesc, mappedResource, pFrame, m_fSdrWhiteScale))
	{
		m_spContext->Unmap(m_spStagingTexture.Get(), 0);
		m_spDuplication->ReleaseFrame();
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = QStringLiteral("Unsupported desktop texture format: %1")
				.arg(static_cast<unsigned int>(sourceDesc.Format));
		}
		return ErrorCaptureResult;
	}

	m_spContext->Unmap(m_spStagingTexture.Get(), 0);
	if (!composePointer(pFrame, pErrorMessage))
	{
		m_spDuplication->ReleaseFrame();
		return ErrorCaptureResult;
	}
	m_spDuplication->ReleaseFrame();
	return CapturedCaptureResult;
}

bool KDxgiDesktopDuplicator::updatePointerState(const DXGI_OUTDUPL_FRAME_INFO &frameInfo,
	QString *pErrorMessage)
{
	bool bPointerUpdated = false;
	if (frameInfo.LastMouseUpdateTime.QuadPart != 0)
	{
		m_pointerPosition = frameInfo.PointerPosition;
		bPointerUpdated = true;
	}

	if (frameInfo.PointerShapeBufferSize > 0)
	{
		m_vecPointerShapeBuffer.resize(frameInfo.PointerShapeBufferSize);
		UINT nRequiredSize = 0;
		HRESULT hr = m_spDuplication->GetFramePointerShape(
			static_cast<UINT>(m_vecPointerShapeBuffer.size()),
			m_vecPointerShapeBuffer.data(),
			&nRequiredSize,
			&m_pointerShapeInfo);
		if (hr == DXGI_ERROR_MORE_DATA && nRequiredSize > m_vecPointerShapeBuffer.size())
		{
			m_vecPointerShapeBuffer.resize(nRequiredSize);
			hr = m_spDuplication->GetFramePointerShape(
				static_cast<UINT>(m_vecPointerShapeBuffer.size()),
				m_vecPointerShapeBuffer.data(),
				&nRequiredSize,
				&m_pointerShapeInfo);
		}
		if (FAILED(hr))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = hresultMessage(QStringLiteral("GetFramePointerShape failed"), hr);
			return false;
		}
		m_vecPointerShapeBuffer.resize(nRequiredSize);
		bPointerUpdated = true;
	}

	if (bPointerUpdated)
	{
		++m_nPointerUpdateCount;
		if (frameInfo.PointerShapeBufferSize > 0 || m_nPointerUpdateCount % 30 == 0)
		{
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("pointer_update"),
				QStringLiteral("x=%1 y=%2 visible=%3 shapeType=%4 width=%5 height=%6")
					.arg(m_pointerPosition.Position.x)
					.arg(m_pointerPosition.Position.y)
					.arg(m_pointerPosition.Visible ? 1 : 0)
					.arg(m_pointerShapeInfo.Type)
					.arg(m_pointerShapeInfo.Width)
					.arg(m_pointerShapeInfo.Height));
		}
	}

	return true;
}

bool KDxgiDesktopDuplicator::composePointer(KCaptureFrame *pFrame, QString *pErrorMessage) const
{
	if (!m_pointerPosition.Visible || m_vecPointerShapeBuffer.empty())
		return true;

	const bool bMonochrome = m_pointerShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
	const int nShapeHeight = static_cast<int>(m_pointerShapeInfo.Height);
	const int nPointerHeight = bMonochrome ? nShapeHeight / 2 : nShapeHeight;
	const int nPointerWidth = static_cast<int>(m_pointerShapeInfo.Width);
	const int nPitch = static_cast<int>(m_pointerShapeInfo.Pitch);
	if (nPointerWidth <= 0 || nPointerHeight <= 0 || nPitch <= 0)
		return true;

	const size_t nRequiredSize = static_cast<size_t>(nPitch) * static_cast<size_t>(nShapeHeight);
	if (nRequiredSize > m_vecPointerShapeBuffer.size())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("DXGI pointer shape buffer is incomplete");
		return false;
	}

	const int nDrawX = m_pointerPosition.Position.x - static_cast<int>(m_pointerShapeInfo.HotSpot.x);
	const int nDrawY = m_pointerPosition.Position.y - static_cast<int>(m_pointerShapeInfo.HotSpot.y);
	for (int y = 0; y < nPointerHeight; ++y)
	{
		const int nTargetY = nDrawY + y;
		if (nTargetY < 0 || nTargetY >= pFrame->nHeight)
			continue;

		for (int x = 0; x < nPointerWidth; ++x)
		{
			const int nTargetX = nDrawX + x;
			if (nTargetX < 0 || nTargetX >= pFrame->nWidth)
				continue;

			unsigned char *pTarget = pFrame->vecBgraBuffer.data()
				+ (static_cast<size_t>(nTargetY) * static_cast<size_t>(pFrame->nWidth)
					+ static_cast<size_t>(nTargetX)) * 4;
			if (bMonochrome)
			{
				const unsigned char *pAndRow = m_vecPointerShapeBuffer.data()
					+ static_cast<size_t>(y) * static_cast<size_t>(nPitch);
				const unsigned char *pXorRow = m_vecPointerShapeBuffer.data()
					+ static_cast<size_t>(y + nPointerHeight) * static_cast<size_t>(nPitch);
				const unsigned char nAndMask = MaskBit(pAndRow, x) != 0 ? 0xff : 0x00;
				const unsigned char nXorMask = MaskBit(pXorRow, x) != 0 ? 0xff : 0x00;
				for (int nChannel = 0; nChannel < 3; ++nChannel)
					pTarget[nChannel] = static_cast<unsigned char>((pTarget[nChannel] & nAndMask) ^ nXorMask);
				continue;
			}

			const unsigned char *pSource = m_vecPointerShapeBuffer.data()
				+ static_cast<size_t>(y) * static_cast<size_t>(nPitch)
				+ static_cast<size_t>(x) * 4;
			if (m_pointerShapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR)
			{
				for (int nChannel = 0; nChannel < 3; ++nChannel)
				{
					pTarget[nChannel] = pSource[3] != 0
						? static_cast<unsigned char>(pTarget[nChannel] ^ pSource[nChannel])
						: pSource[nChannel];
				}
				continue;
			}

			if (m_pointerShapeInfo.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR)
				continue;
			const unsigned int nInverseAlpha = 255u - pSource[3];
			for (int nChannel = 0; nChannel < 3; ++nChannel)
			{
				pTarget[nChannel] = static_cast<unsigned char>(std::min(255u,
					static_cast<unsigned int>(pSource[nChannel])
						+ static_cast<unsigned int>(pTarget[nChannel]) * nInverseAlpha / 255u));
			}
		}
	}

	return true;
}

bool KDxgiDesktopDuplicator::createStagingTexture(const D3D11_TEXTURE2D_DESC &sourceDesc, QString *pErrorMessage)
{
	if (m_spStagingTexture
		&& m_stagingDesc.Width == sourceDesc.Width
		&& m_stagingDesc.Height == sourceDesc.Height
		&& m_stagingDesc.Format == sourceDesc.Format)
	{
		return true;
	}

	D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
	stagingDesc.BindFlags = 0;
	stagingDesc.MiscFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.MipLevels = 1;
	stagingDesc.ArraySize = 1;
	stagingDesc.SampleDesc.Count = 1;
	stagingDesc.SampleDesc.Quality = 0;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> spTexture;
	const HRESULT hr = m_spDevice->CreateTexture2D(&stagingDesc, nullptr, &spTexture);
	if (FAILED(hr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = hresultMessage(QStringLiteral("Create staging texture failed"), hr);
		return false;
	}

	m_spStagingTexture = spTexture;
	m_stagingDesc = stagingDesc;
	return true;
}

QString KDxgiDesktopDuplicator::hresultMessage(const QString &strPrefix, HRESULT hr)
{
	return QStringLiteral("%1 (HRESULT=0x%2)")
		.arg(strPrefix)
		.arg(static_cast<unsigned int>(hr), 8, 16, QLatin1Char('0'));
}
