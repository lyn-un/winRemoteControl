#include "codec/h264encoder.h"
#include "h264hardwarebitratetest.h"

#include <QtCore/QCoreApplication>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <iostream>

extern "C"
{
#include <libavutil/log.h>
}

bool TestHardwareResources(KVideoEncoderPreference preference);
bool TestHardwareLevels(KVideoEncoderPreference preference, bool bStartupRates);

int main(int nArgc, char **pArgv)
{
	QCoreApplication application(nArgc, pArgv);
	if (qEnvironmentVariableIntValue("WRC_RUN_VENDOR_BITRATE_TEST") != 1)
		return 77;
	if (application.arguments().contains(QStringLiteral("--verbose")))
		av_log_set_level(AV_LOG_VERBOSE);
	const QString strBackend = application.arguments().value(1);
	if (strBackend != QStringLiteral("h264_qsv") && strBackend != QStringLiteral("h264_amf")
		&& strBackend != QStringLiteral("h264_nvenc"))
		return 1;
	const bool bAmf = strBackend == QStringLiteral("h264_amf");
	const bool bNvenc = strBackend == QStringLiteral("h264_nvenc");
	KNvencReconfiguration configuration;
	bool bResetSpecified = false, bIdrSpecified = false;
	for (const QString &strArgument : application.arguments())
	{
		const bool bReset = strArgument.startsWith(QStringLiteral("--nvenc-reconfig-reset="));
		const bool bIdr = strArgument.startsWith(QStringLiteral("--nvenc-reconfig-idr="));
		if (!bReset && !bIdr)
		{
			if (strArgument.startsWith(QStringLiteral("--nvenc-reconfig-")))
				return 2;
			continue;
		}
		const QString strValue = strArgument.section(QLatin1Char('='), 1);
		if (!bNvenc || (strValue != QStringLiteral("0") && strValue != QStringLiteral("1"))
			|| (bReset ? bResetSpecified : bIdrSpecified))
			return 2;
		configuration.bOverride = true;
		if (bReset)
		{
			bResetSpecified = true;
			configuration.bReset = strValue == QStringLiteral("1");
		}
		else
		{
			bIdrSpecified = true;
			configuration.bForceIdr = strValue == QStringLiteral("1");
		}
	}
	if (bResetSpecified != bIdrSpecified || (configuration.bOverride
		&& (application.arguments().contains(QStringLiteral("--resources"))
			|| application.arguments().contains(QStringLiteral("--levels"))
			|| application.arguments().contains(QStringLiteral("--startup-rates")))))
		return 2;
	std::cout << "nvencOverride=" << configuration.bOverride << " reset=" << configuration.bReset
		<< " forceIdr=" << configuration.bForceIdr << std::endl;
	const UINT nVendorId = bNvenc ? 0x10de : (bAmf ? 0x1002 : 0x8086);
	const KVideoEncoderPreference preference = bNvenc ? NvencVideoEncoderPreference
		: (bAmf ? AmfVideoEncoderPreference : QuickSyncVideoEncoderPreference);
	Microsoft::WRL::ComPtr<IDXGIFactory1> spFactory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&spFactory))))
		return 1;
	bool bFound = false;
	for (UINT nIndex = 0; ; ++nIndex)
	{
		Microsoft::WRL::ComPtr<IDXGIAdapter1> spAdapter;
		const HRESULT hEnum = spFactory->EnumAdapters1(nIndex, &spAdapter);
		if (hEnum == DXGI_ERROR_NOT_FOUND)
			break;
		if (FAILED(hEnum))
			return 1;
		DXGI_ADAPTER_DESC1 descriptor = {};
		if (FAILED(spAdapter->GetDesc1(&descriptor)))
			return 1;
		if (descriptor.VendorId != nVendorId || (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
			continue;
		bFound = true;
		LARGE_INTEGER version = {};
		const HRESULT hVersion = spAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
		std::cout << "candidateGpu=" << QString::fromWCharArray(descriptor.Description).toStdString()
			<< " vendorId=" << descriptor.VendorId << " deviceId=" << descriptor.DeviceId
			<< " driverVersionRaw=" << version.QuadPart << " versionResult=" << hVersion << '\n';
	}
	if (!bFound)
	{
		KH264Encoder encoder(preference);
		QString strError;
		if (encoder.openStream(320, 240, 30, 1000, [](const QByteArray &) {}, &strError))
		{
			std::cerr << "Unexpected encoder opened without matching hardware: "
				<< encoder.encoderName().toStdString() << '\n';
			return 1;
		}
		std::cout << "Missing hardware rejected without fallback: " << strError.toStdString()
			<< "\nHardware acceptance skipped\n";
		return 77;
	}
	const HRESULT hCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hCom))
		return 1;
	const bool bPassed = application.arguments().contains(QStringLiteral("--keyframe-policy"))
		? bNvenc && TestNvencKeyframePolicy(configuration)
		: application.arguments().contains(QStringLiteral("--frequent-rates"))
		? TestHardwareFrequentRates(preference, configuration)
		: application.arguments().contains(QStringLiteral("--resources"))
		? TestHardwareResources(preference)
		: application.arguments().contains(QStringLiteral("--levels"))
			|| application.arguments().contains(QStringLiteral("--startup-rates"))
			? TestHardwareLevels(preference, application.arguments().contains(QStringLiteral("--startup-rates")))
			: TestHardwareBitrateOutput(preference, configuration);
	CoUninitialize();
	return bPassed ? 0 : 1;
}
