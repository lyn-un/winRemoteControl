#ifndef _WINREMOTECONTROL_WEBRTCCALLBACKGATE_H_
#define _WINREMOTECONTROL_WEBRTCCALLBACKGATE_H_

#include <QtCore/QtGlobal>

#include <functional>
#include <memory>
#include <mutex>

class QObject;

class KWebRtcCallbackGate : public std::enable_shared_from_this<KWebRtcCallbackGate>
{
public:
	using Callback = std::function<void(QObject *)>;

	KWebRtcCallbackGate() = default;

	KWebRtcCallbackGate(const KWebRtcCallbackGate &) = delete;
	KWebRtcCallbackGate &operator=(const KWebRtcCallbackGate &) = delete;

	void open(QObject *pTarget, quint64 nGeneration);
	void close();
	bool post(quint64 nGeneration, Callback callback);

private:
	bool isCurrent(QObject *pTarget, quint64 nGeneration) const;

	mutable std::mutex m_mutex;
	QObject *m_pTarget = nullptr;
	quint64 m_nGeneration = 0;
};

#endif // _WINREMOTECONTROL_WEBRTCCALLBACKGATE_H_
