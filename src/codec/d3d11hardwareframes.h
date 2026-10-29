#ifndef _WINREMOTECONTROL_D3D11HARDWAREFRAMES_H_
#define _WINREMOTECONTROL_D3D11HARDWAREFRAMES_H_

#include <QtCore/QString>
#include <QtCore/QtGlobal>

#include <memory>
#include <mutex>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct AVBufferRef;
struct AVFrame;

class KD3D11HardwareFrame
{
public:
	~KD3D11HardwareFrame();
	KD3D11HardwareFrame(const KD3D11HardwareFrame &) = delete;
	KD3D11HardwareFrame &operator=(const KD3D11HardwareFrame &) = delete;

	ID3D11Texture2D *texture() const;
	AVFrame *encoderFrame() const;
	quint64 generation() const;

private:
	friend class KD3D11HardwareFrames;
	KD3D11HardwareFrame(AVFrame *pSource, AVFrame *pEncoder, quint64 nGeneration);
	AVFrame *m_pSource = nullptr;
	AVFrame *m_pEncoder = nullptr;
	quint64 m_nGeneration = 0;
};

// Windows/FFmpeg adapter. The pool is bounded; AVFrame references, not COM
// reference counts, determine when an encoder releases a slot for reuse.
// Pool/lifecycle methods run on one producer thread. The context lock is shared
// with FFmpeg; lifecycle must not change while a caller holds that lock.
class KD3D11HardwareFrames
{
public:
	enum AcquireResult
	{
		AcquiredResult,
		PoolBusyResult,
		FailedResult
	};
	KD3D11HardwareFrames();
	~KD3D11HardwareFrames();
	KD3D11HardwareFrames(const KD3D11HardwareFrames &) = delete;
	KD3D11HardwareFrames &operator=(const KD3D11HardwareFrames &) = delete;

	bool initialize(ID3D11Device *pDevice, const QString &strEncoder,
		int nWidth, int nHeight, quint64 nGeneration, QString *pError,
		std::shared_ptr<std::recursive_mutex> spContextMutex = nullptr);
	AcquireResult acquire(std::shared_ptr<KD3D11HardwareFrame> *pFrame, QString *pError);
	int capacity() const;
	// Observed at acquire boundaries; asynchronous releases can race the scan.
	int peakInUse() const;
	void reset();
	// Borrowed contexts; callers retaining them past reset must av_buffer_ref().
	AVBufferRef *deviceContext() const;
	AVBufferRef *framesContext() const;
	ID3D11DeviceContext *immediateContext() const;
	void lockContext();
	void unlockContext();

private:
	struct KState;
	std::unique_ptr<KState> m_upState;
};

#endif // _WINREMOTECONTROL_D3D11HARDWAREFRAMES_H_
