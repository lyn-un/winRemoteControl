#ifndef _WINREMOTECONTROL_SESSION_ACCESSSESSIONFLOW_H_
#define _WINREMOTECONTROL_SESSION_ACCESSSESSIONFLOW_H_

#include "core/protocol/accessmessage.h"

#include <QtCore/QObject>

class KSignalingTransport;

class KAccessSessionFlow final : public QObject
{
	Q_OBJECT

public:
	explicit KAccessSessionFlow(KSignalingTransport *pTransport,
		QObject *pParent = nullptr);
	bool startListening(quint16 nPort, QString *pErrorMessage);
	void connectToHost(const QString &strHost, quint16 nPort);
	void disconnectPeer();
	void stop();
	void sendAccessMessage(const KAccessMessage &message);
	void sendSignalingMessage(const QString &strMessage);
	void setConnected(bool bConnected);
	bool isConnected() const;
	bool matchesEndpoint(const QString &strHost, quint16 nPort) const;
	bool hasLastEndpoint() const;
	QString lastHost() const;
	quint16 lastPort() const;
	quint16 listeningPort() const;
	void clearLastEndpoint();

signals:
	void messageReceived(const QString &strMessage);
	void stateChanged(const QString &strState);
	void signalingError(const QString &strMessage);
	void outgoingConnectionEstablished();
	void outgoingConnectionFailed(const QString &strMessage);
	void incomingConnectionEstablished(const QString &strSourceAddress,
		quint16 nSourcePort);
	void connectionLost();

private:
	KSignalingTransport *m_pTransport = nullptr;
	QString m_strLastHost;
	quint16 m_nLastPort = 0;
	quint16 m_nListeningPort = 0;
	bool m_bConnected = false;
};

#endif // _WINREMOTECONTROL_SESSION_ACCESSSESSIONFLOW_H_
