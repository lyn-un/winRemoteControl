#ifndef _WINREMOTECONTROL_MAPPEDFRAMECONVERSION_H_
#define _WINREMOTECONTROL_MAPPEDFRAMECONVERSION_H_

#include "core/media/capturesource.h"
#include <d3d11.h>

// Copies a mapped capture surface, preserving the legacy finite-value conversion.
bool CopyMappedCaptureFrameToBgra(const D3D11_TEXTURE2D_DESC &sourceDesc,
	const D3D11_MAPPED_SUBRESOURCE &mappedResource, KCaptureFrame *pFrame, float fSdrWhiteScale);

#endif // _WINREMOTECONTROL_MAPPEDFRAMECONVERSION_H_
