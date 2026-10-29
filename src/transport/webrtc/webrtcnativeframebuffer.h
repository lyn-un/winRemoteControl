#ifndef _WINREMOTECONTROL_WEBRTCNATIVEFRAMEBUFFER_H_
#define _WINREMOTECONTROL_WEBRTCNATIVEFRAMEBUFFER_H_

#include "core/media/nativevideoframe.h"
#include "core/media/videoframepath.h"

#include <api/video/video_frame_buffer.h>

#include <memory>
#include <mutex>

class KWebRtcNativeFrameBuffer : public webrtc::VideoFrameBuffer
{
public:
	KWebRtcNativeFrameBuffer(std::shared_ptr<IKNativeVideoFrame> spFrame,
		std::shared_ptr<KVideoFramePathState> spPath, int nWidth, int nHeight);
	Type type() const override { return Type::kNative; }
	int width() const override { return m_nWidth; }
	int height() const override { return m_nHeight; }
	webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
	webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(int nOffsetX, int nOffsetY,
		int nCropWidth, int nCropHeight, int nScaledWidth, int nScaledHeight) override;
	std::shared_ptr<IKNativeVideoFrame> nativeFrame() const { return m_spFrame; }

private:
	std::shared_ptr<IKNativeVideoFrame> m_spFrame;
	std::shared_ptr<KVideoFramePathState> m_spPath;
	int m_nWidth = 0;
	int m_nHeight = 0;
	std::mutex m_mutex;
	webrtc::scoped_refptr<webrtc::I420BufferInterface> m_spI420;
};

#endif // _WINREMOTECONTROL_WEBRTCNATIVEFRAMEBUFFER_H_
