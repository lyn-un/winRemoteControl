#include "codec/h264encoder.h"
#include <QtCore/QCoreApplication>
#include <iostream>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>

bool TestHardwareBitrateOutput(KVideoEncoderPreference preference);

int main(int nArgc, char **pArgv)
{
	QCoreApplication application(nArgc, pArgv);
	if (qEnvironmentVariableIntValue("WRC_RUN_MF_BITRATE_TEST") != 1)
		return 77;
	// Only skip when no hardware encoder is enumerated, never on a bitrate failure.
	const HRESULT hCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hCom))
		return 1;
	if (FAILED(MFStartup(MF_VERSION)))
	{
		CoUninitialize();
		return 1;
	}
	MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_H264 };
	IMFActivate **ppActivations = nullptr;
	UINT32 nCount = 0;
	const HRESULT hEnum = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE,
		nullptr, &output, &ppActivations, &nCount);
	for (UINT32 nIndex = 0; nIndex < nCount; ++nIndex)
		ppActivations[nIndex]->Release();
	CoTaskMemFree(ppActivations);
	MFShutdown();
	CoUninitialize();
	if (FAILED(hEnum))
		return 1;
	if (!nCount)
	{
		std::cout << "No hardware H.264 MFT available; skipped" << std::endl;
		return 77;
	}
	return TestHardwareBitrateOutput(MediaFoundationVideoEncoderPreference) ? 0 : 1;
}
