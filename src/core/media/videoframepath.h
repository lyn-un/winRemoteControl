#ifndef _WINREMOTECONTROL_VIDEOFRAMEPATH_H_
#define _WINREMOTECONTROL_VIDEOFRAMEPATH_H_

#include <QtCore/QString>

#include <mutex>

enum KVideoFramePathMode
{
	AutoVideoFramePath,
	CpuVideoFramePath,
	GpuVideoFramePath
};

inline const char *VideoFramePathName(KVideoFramePathMode mode)
{
	switch (mode)
	{
	case AutoVideoFramePath: return "auto";
	case GpuVideoFramePath: return "gpu";
	default: return "cpu";
	}
}

// Per-session decisions shared by capture and WebRTC's encoder thread. Old
// generations cannot force a new session into fallback or failure.
class KVideoFramePathState
{
public:
	enum Decision
	{
		Undecided,
		Gpu,
		Cpu,
		Failed
	};
	struct KSnapshot
	{
		quint64 nGeneration = 0;
		Decision decision = Undecided;
		bool bSeparateCursor = false;
		QString strReason;
	};
	explicit KVideoFramePathState(KVideoFramePathMode mode = CpuVideoFramePath)
		: m_mode(mode)
	{
	}
	void begin(quint64 nGeneration)
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_snapshot.nGeneration == nGeneration)
			return;
		m_snapshot = {};
		m_snapshot.nGeneration = nGeneration;
		if (m_mode == CpuVideoFramePath)
			m_snapshot.decision = Cpu;
	}
	KSnapshot snapshot() const
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		return m_snapshot;
	}
	void setSeparateCursor(quint64 nGeneration, bool bEnabled)
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_snapshot.nGeneration == nGeneration)
			m_snapshot.bSeparateCursor = bEnabled;
	}
	void fallback(quint64 nGeneration, const QString &strReason)
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_snapshot.nGeneration != nGeneration || m_snapshot.decision == Cpu || m_snapshot.decision == Failed)
			return;
		m_snapshot.decision = m_mode == GpuVideoFramePath || m_snapshot.decision == Gpu ? Failed : Cpu;
		m_snapshot.strReason = strReason;
	}
	void fail(quint64 nGeneration, const QString &strReason)
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_snapshot.nGeneration != nGeneration)
			return;
		m_snapshot.decision = Failed;
		m_snapshot.strReason = strReason;
	}
	void selectGpu(quint64 nGeneration)
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_snapshot.nGeneration == nGeneration && m_snapshot.decision == Undecided)
			m_snapshot.decision = Gpu;
	}
	KVideoFramePathMode mode() const { return m_mode; }

private:
	const KVideoFramePathMode m_mode;
	mutable std::mutex m_mutex;
	KSnapshot m_snapshot;
};

#endif // _WINREMOTECONTROL_VIDEOFRAMEPATH_H_
