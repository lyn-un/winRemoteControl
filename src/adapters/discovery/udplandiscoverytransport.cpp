#include "adapters/discovery/udplandiscoverytransport.h"

#if defined(Q_OS_WIN)
#include <WinSock2.h>
#include <Mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

#include <QtCore/QSet>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkDatagram>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QUdpSocket>

namespace
{
	void disableUdpConnectionReset(QUdpSocket *pSocket)
	{
#if defined(Q_OS_WIN)
		BOOL bNewBehavior = FALSE;
		DWORD nBytesReturned = 0;
		::WSAIoctl(static_cast<SOCKET>(pSocket->socketDescriptor()),
			SIO_UDP_CONNRESET,
			&bNewBehavior,
			sizeof(bNewBehavior),
			nullptr,
			0,
			&nBytesReturned,
			nullptr,
			nullptr);
#else
		Q_UNUSED(pSocket);
#endif
	}
}

KUdpLanDiscoveryTransport::KUdpLanDiscoveryTransport(QObject *pParent)
	: KLanDiscoveryTransport(pParent)
	, m_pSocket(new QUdpSocket(this))
{
	connect(m_pSocket, &QUdpSocket::readyRead,
		this, &KUdpLanDiscoveryTransport::readPendingDatagrams);
	connect(m_pSocket, &QUdpSocket::errorOccurred,
		this, &KUdpLanDiscoveryTransport::handleSocketError);
}

bool KUdpLanDiscoveryTransport::start(quint16 nLocalPort, QString *pErrorMessage)
{
	m_pSocket->close();
	const bool bBound = m_pSocket->bind(QHostAddress::AnyIPv4,
		nLocalPort,
		QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
	if (!bBound)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = m_pSocket->errorString();
		return false;
	}
	disableUdpConnectionReset(m_pSocket);
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

void KUdpLanDiscoveryTransport::sendBroadcast(const QByteArray &data, quint16 nPort)
{
	QSet<QHostAddress> addresses;
	for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces())
	{
		const auto flags = networkInterface.flags();
		if (!flags.testFlag(QNetworkInterface::IsUp)
			|| !flags.testFlag(QNetworkInterface::IsRunning)
			|| !flags.testFlag(QNetworkInterface::CanBroadcast))
		{
			continue;
		}
		for (const QNetworkAddressEntry &entry : networkInterface.addressEntries())
		{
			if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
				&& !entry.broadcast().isNull())
			{
				addresses.insert(entry.broadcast());
			}
		}
	}
	addresses.insert(QHostAddress::LocalHost);
	for (const QHostAddress &address : addresses)
		sendDatagram(data, address, nPort);
}

void KUdpLanDiscoveryTransport::sendUnicast(const QByteArray &data,
	const QString &strHost,
	quint16 nPort)
{
	const QHostAddress address(strHost);
	if (address.protocol() != QAbstractSocket::IPv4Protocol)
	{
		emit transportError(QStringLiteral("Invalid IPv4 discovery target"));
		return;
	}
	sendDatagram(data, address, nPort);
}

void KUdpLanDiscoveryTransport::stop()
{
	m_pSocket->close();
}

void KUdpLanDiscoveryTransport::readPendingDatagrams()
{
	while (m_pSocket->hasPendingDatagrams())
	{
		const QNetworkDatagram datagram = m_pSocket->receiveDatagram();
		if (!datagram.isValid())
			continue;
		emit datagramReceived(datagram.data(),
			datagram.senderAddress().toString(),
			datagram.senderPort());
	}
}

void KUdpLanDiscoveryTransport::handleSocketError()
{
	if (m_pSocket->error() != QAbstractSocket::UnknownSocketError)
		emit transportError(m_pSocket->errorString());
}

void KUdpLanDiscoveryTransport::sendDatagram(const QByteArray &data,
	const QHostAddress &address,
	quint16 nPort)
{
	if (m_pSocket->state() != QAbstractSocket::BoundState)
		return;
	if (m_pSocket->writeDatagram(data, address, nPort) < 0)
		emit transportError(m_pSocket->errorString());
}
