#ifndef _WINREMOTECONTROL_NATIVEVIDEOFRAME_H_
#define _WINREMOTECONTROL_NATIVEVIDEOFRAME_H_

#include <QtCore/QtGlobal>

class IKNativeVideoFrame
{
public:
	enum NativeType
	{
		D3D11BgraNativeType
	};
	virtual ~IKNativeVideoFrame() = default;
	virtual NativeType nativeType() const = 0;
	virtual int width() const = 0;
	virtual int height() const = 0;
	virtual quint64 generation() const = 0;
	virtual quint64 adapterLuid() const = 0;
};

#endif // _WINREMOTECONTROL_NATIVEVIDEOFRAME_H_
