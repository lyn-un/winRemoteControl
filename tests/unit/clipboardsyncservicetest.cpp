#include "clipboard/clipboardsyncservice.h"

#include "core/clipboard/clipboardadapter.h"
#include "session/sessioncontroller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

#include <iostream>
#include <memory>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	class KFakeClipboardAdapter final : public KClipboardAdapter
	{
	public:
		QString text() const override
		{
			return strText;
		}

		bool setText(const QString &strValue, QString *) override
		{
			++nSetCount;
			vecSetValues.append(strValue);
			if (nFailuresRemaining > 0)
			{
				--nFailuresRemaining;
				return false;
			}
			strText = strValue;
			return true;
		}

		void copyText(const QString &strValue)
		{
			strText = strValue;
			emit textChanged(strText);
		}

		QString strText;
		int nSetCount = 0;
		int nFailuresRemaining = 0;
		QVector<QString> vecSetValues;
	};

	void ProcessEventsFor(int nMilliseconds)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < nMilliseconds)
		{
			QCoreApplication::processEvents();
			QThread::msleep(5);
		}
		QCoreApplication::processEvents();
	}

	class KFakeSessionController final : public KSessionController
	{
	public:
		void setRole(const QString &) override {}
		void startSignalingServer(quint16) override {}
		void connectSignaling(const QString &, quint16) override {}
		void retryLastConnection() override {}
		void disconnectSession() override {}
		void enterRemoteDesktop(const KStreamConfig &) override {}
		void leaveRemoteDesktop() override {}
		void startStreaming() override {}
		void stopStreaming() override {}
		void pushVideoFrame(const KVideoFrame &) override {}
		void sendInputMessage(const KInputMessage &) override {}
		void sendStreamConfig(const KStreamConfig &) override {}
		void handleCaptureFailure() override {}
		void applyApplicationSettings(const KApplicationSettings &) override {}
		void respondIncomingAccessRequest(const QString &, bool) override {}
		void respondPairingRequest(const QString &, bool,
			KPermissionScopes) override {}
		quint64 sessionGeneration() const override { return 1; }
		bool isIdle() const override { return false; }
		bool matchesCurrentEndpoint(const QString &, quint16) const override { return false; }

		void sendClipboardMessage(const KClipboardMessage &message) override
		{
			vecSentMessages.append(message);
		}
		bool sendTerminalControlMessage(const KTerminalMessage &) override { return false; }
		bool sendTerminalData(const QByteArray &) override { return false; }
		bool isTerminalBackpressured() const override { return false; }

		void changeState(const QString &strState)
		{
			if (strState == QStringLiteral("Connected"))
				emit sessionStateChanged(ConnectedSessionState);
			else if (strState == QStringLiteral("Streaming"))
				emit sessionStateChanged(StreamingSessionState);
			else if (strState == QStringLiteral("Reconnecting"))
				emit sessionStateChanged(ReconnectingSessionState);
			else if (strState == QStringLiteral("Disconnected"))
				emit sessionStateChanged(IdleSessionState);
			else
				emit webRtcStateChanged(strState);
		}

		void changeClipboardChannel(bool bOpen)
		{
			emit clipboardChannelChanged(bOpen);
		}

		void deliver(const KClipboardMessage &message)
		{
			emit clipboardMessageReceived(message);
		}

		QVector<KClipboardMessage> vecSentMessages;
	};

	KClipboardMessage TextMessage(const QString &strId, const QString &strText)
	{
		KClipboardMessage message;
		message.type = TextClipboardMessageType;
		message.strMessageId = strId;
		message.strText = strText;
		return message;
	}

	KClipboardMessage ReadyMessage(const QString &strId)
	{
		KClipboardMessage message;
		message.type = ReadyClipboardMessageType;
		message.strMessageId = strId;
		return message;
	}

	void TestClipboardSyncLifecycle()
	{
		auto spAdapter = std::make_unique<KFakeClipboardAdapter>();
		KFakeClipboardAdapter *pAdapter = spAdapter.get();
		KFakeSessionController controller;
		KClipboardSyncService service(std::move(spAdapter), &controller);

		controller.changeState(QStringLiteral("Connected"));
		controller.changeClipboardChannel(true);
		controller.changeState(QStringLiteral("Streaming"));
		Check(controller.vecSentMessages.size() == 1
			&& controller.vecSentMessages.constLast().type == ReadyClipboardMessageType,
			QStringLiteral("clipboard capability is announced after streaming starts"));
		pAdapter->copyText(QStringLiteral("before peer ready"));
		Check(controller.vecSentMessages.size() == 1,
			QStringLiteral("local text waits for peer capability confirmation"));
		controller.deliver(ReadyMessage(
			QStringLiteral("aaaaaaaa-1234-1234-1234-1234567890ab")));
		controller.vecSentMessages.clear();
		pAdapter->copyText(QStringLiteral("local one"));
		Check(controller.vecSentMessages.size() == 1
			&& controller.vecSentMessages.constLast().type == TextClipboardMessageType,
			QStringLiteral("new local text is sent while streaming"));
		pAdapter->copyText(QString());
		Check(controller.vecSentMessages.size() == 1,
			QStringLiteral("empty local clipboard text is not sent"));
		controller.changeState(QStringLiteral("connected"));
		pAdapter->copyText(QStringLiteral("after raw state"));
		Check(controller.vecSentMessages.size() == 2,
			QStringLiteral("raw WebRTC states do not override streaming permission"));

		const QString strRemoteId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		controller.deliver(TextMessage(strRemoteId, QStringLiteral("remote text")));
		Check(pAdapter->strText == QStringLiteral("remote text") && pAdapter->nSetCount == 1,
			QStringLiteral("remote text is applied once"));
		Check(controller.vecSentMessages.size() == 2,
			QStringLiteral("remote clipboard write is not echoed"));
		controller.deliver(TextMessage(strRemoteId, QStringLiteral("duplicate")));
		Check(pAdapter->nSetCount == 1, QStringLiteral("duplicate message ID is ignored"));
		controller.deliver(TextMessage(
			QStringLiteral("87654321-4321-4321-4321-ba0987654321"), QString()));
		Check(pAdapter->nSetCount == 1,
			QStringLiteral("empty remote clipboard text is not applied"));

		pAdapter->nFailuresRemaining = 1;
		const QString strRetryId = QStringLiteral("11111111-2222-3333-4444-555555555555");
		controller.deliver(TextMessage(strRetryId, QStringLiteral("retry text")));
		ProcessEventsFor(150);
		Check(pAdapter->nSetCount == 3,
			QStringLiteral("a failed clipboard write is retried once"));
		Check(pAdapter->vecSetValues.size() >= 3
			&& pAdapter->vecSetValues.at(1) == QStringLiteral("retry text")
			&& pAdapter->vecSetValues.at(2) == QStringLiteral("retry text"),
			QStringLiteral("clipboard retry preserves the original non-empty text"));
		Check(pAdapter->strText == QStringLiteral("retry text"),
			QStringLiteral("clipboard retry applies the original text"));

		QCoreApplication::processEvents();
		pAdapter->copyText(QStringLiteral("remote text"));
		Check(controller.vecSentMessages.size() == 3,
			QStringLiteral("user can copy the same text again"));

		service.setEnabled(false);
		Check(controller.vecSentMessages.constLast().type == SyncStateClipboardMessageType
			&& !controller.vecSentMessages.constLast().bEnabled,
			QStringLiteral("disabling sync is sent to the remote peer"));
		const int nSentAfterDisable = controller.vecSentMessages.size();
		pAdapter->copyText(QStringLiteral("not sent"));
		controller.deliver(TextMessage(
			QStringLiteral("abcdefab-1234-5678-9abc-def012345678"),
			QStringLiteral("not applied")));
		Check(controller.vecSentMessages.size() == nSentAfterDisable
			&& pAdapter->strText == QStringLiteral("not sent"),
			QStringLiteral("disabled sync does not send or apply text"));

		service.setEnabled(true);
		controller.changeState(QStringLiteral("Reconnecting"));
		const int nSentBeforeReconnectCopy = controller.vecSentMessages.size();
		pAdapter->copyText(QStringLiteral("during reconnect"));
		Check(controller.vecSentMessages.size() == nSentBeforeReconnectCopy,
			QStringLiteral("reconnecting does not queue local clipboard text"));
		controller.changeState(QStringLiteral("Streaming"));
		pAdapter->copyText(QStringLiteral("after reconnect"));
		Check(controller.vecSentMessages.size() == nSentBeforeReconnectCopy + 1,
			QStringLiteral("new text resumes after recovery"));

		controller.changeState(QStringLiteral("Disconnected"));
		controller.changeState(QStringLiteral("Connected"));
		controller.changeClipboardChannel(true);
		controller.changeState(QStringLiteral("Streaming"));
		controller.deliver(ReadyMessage(
			QStringLiteral("bbbbbbbb-1234-1234-1234-1234567890ab")));
		pAdapter->copyText(QStringLiteral("new session"));
		Check(controller.vecSentMessages.constLast().type == TextClipboardMessageType,
			QStringLiteral("new sessions restore the enabled default"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestClipboardSyncLifecycle();
	if (g_nFailureCount == 0)
		qInfo() << "All clipboard sync service tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
