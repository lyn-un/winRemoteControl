#include "capture/dxgidesktopduplicator.h"
#include "tools/diagnostics/resourceprobecommon.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QThread>

#include <d3d11.h>
#include <wrl/client.h>

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	QCommandLineParser parser;
	parser.addHelpOption();
	parser.addOption(QCommandLineOption(QStringLiteral("cycles"),
		QStringLiteral("Number of capture create/destroy cycles."),
		QStringLiteral("count"), QStringLiteral("5")));
	parser.addOption(QCommandLineOption(QStringLiteral("frames"),
		QStringLiteral("Number of desktop frames captured in each cycle."),
		QStringLiteral("count"), QStringLiteral("10")));
	parser.addOption(QCommandLineOption(QStringLiteral("final-only"),
		QStringLiteral("Only sample resources before and after all capture cycles.")));
	parser.addOption(QCommandLineOption(QStringLiteral("device-only"),
		QStringLiteral("Create and release only the D3D11 device and immediate context.")));
	parser.process(application);
	bool bCyclesValid = false;
	const int nCycles = parser.value(QStringLiteral("cycles")).toInt(&bCyclesValid);
	bool bFramesValid = false;
	const int nFrames = parser.value(QStringLiteral("frames")).toInt(&bFramesValid);
	if (!bCyclesValid || nCycles < 1 || nCycles > 100
		|| !bFramesValid || nFrames < 0 || nFrames > 100)
		return 2;

	PrintResourceProbeSnapshot(QStringLiteral("capture"), 0, QStringLiteral("baseline"));
	for (int nCycle = 1; nCycle <= nCycles; ++nCycle)
	{
		if (parser.isSet(QStringLiteral("device-only")))
		{
			Microsoft::WRL::ComPtr<ID3D11Device> spDevice;
			Microsoft::WRL::ComPtr<ID3D11DeviceContext> spContext;
			D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
			const D3D_FEATURE_LEVEL featureLevels[] = {
				D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
			};
			const HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
				nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 2,
				D3D11_SDK_VERSION, &spDevice, &featureLevel, &spContext);
			if (FAILED(hr))
				return 3;
			spContext->ClearState();
			spContext->Flush();
		}
		else
		{
			KDxgiDesktopDuplicator duplicator;
			QString strError;
			if (!duplicator.initialize(&strError))
			{
				qCritical().noquote() << strError;
				return 3;
			}

			int nCapturedFrames = 0;
			for (int nAttempt = 0; nAttempt < 100 && nCapturedFrames < nFrames; ++nAttempt)
			{
				KCaptureFrame frame;
				const IKCaptureSource::CaptureResult result =
					duplicator.captureNextFrame(&frame, &strError);
				if (result == IKCaptureSource::ErrorCaptureResult)
				{
					qCritical().noquote() << strError;
					return 4;
				}
				if (result == IKCaptureSource::CapturedCaptureResult)
					++nCapturedFrames;
			}
			duplicator.shutdown();
		}
		QThread::msleep(500);
		if (!parser.isSet(QStringLiteral("final-only")))
		{
			PrintResourceProbeSnapshot(QStringLiteral("capture"),
				nCycle, QStringLiteral("shutdown"));
		}
	}
	if (parser.isSet(QStringLiteral("final-only")))
		PrintResourceProbeSnapshot(QStringLiteral("capture"), nCycles, QStringLiteral("final"));
	return 0;
}
