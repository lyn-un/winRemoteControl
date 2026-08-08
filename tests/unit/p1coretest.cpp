#include "core/protocol/protocolrouter.h"
#include "core/protocol/inputmessage.h"
#include "core/session/sessionerror.h"
#include "core/transport/outboundmessagequeue.h"
#include "session/sessionerrorpresenter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

namespace
{
	int g_nFailures = 0;

	void Check(bool bCondition, const QString &strMessage)
	{
		if (bCondition)
			return;
		qCritical().noquote() << strMessage;
		++g_nFailures;
	}

	void TestProtocolRouter()
	{
		KProtocolRouter router;
		int nHandled = 0;
		Check(router.registerHandler(InputProtocolChannel,
			QStringLiteral("mouseMove"),
			[](const KProtocolEnvelope &, const KProtocolRouteContext &context)
			{
				return context.nState == 7;
			},
			[&nHandled](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{
				Check(envelope.nSequence == 42, QStringLiteral("router preserves sequence"));
				++nHandled;
				return KProtocolHandlerResult::success();
			}), QStringLiteral("router registers handler"));
		Check(!router.registerHandler(InputProtocolChannel,
			QStringLiteral("mouseMove"), {},
			[](const KProtocolEnvelope &, const KProtocolRouteContext &)
			{ return KProtocolHandlerResult::success(); }),
			QStringLiteral("router rejects duplicate handler"));

		KProtocolRouteContext context;
		context.nState = 7;
		KProtocolRouteResult result = router.route(InputProtocolChannel,
			QStringLiteral("{\"version\":1,\"type\":\"mouseMove\",\"seq\":\"42\","
				"\"payload\":{\"x\":10}}"), context);
		Check(result.status == HandledProtocolRouteStatus && nHandled == 1,
			QStringLiteral("router dispatches known message"));
		Check(result.envelope.payload.value(QStringLiteral("x")).toInt() == 10,
			QStringLiteral("router exposes nested payload"));

		context.nState = 2;
		result = router.route(InputProtocolChannel,
			QStringLiteral("{\"version\":1,\"type\":\"mouseMove\"}"), context);
		Check(result.status == ForbiddenProtocolRouteStatus,
			QStringLiteral("router rejects forbidden state"));
		result = router.route(InputProtocolChannel,
			QStringLiteral("{\"version\":1,\"type\":\"unknown\"}"), context);
		Check(result.status == UnknownTypeProtocolRouteStatus,
			QStringLiteral("router rejects unknown type"));
		result = router.route(InputProtocolChannel,
			QStringLiteral("{\"version\":2,\"type\":\"mouseMove\"}"), context);
		Check(result.status == UnsupportedVersionProtocolRouteStatus,
			QStringLiteral("router rejects unsupported version"));
		result = router.route(InputProtocolChannel, QStringLiteral("not-json"), context);
		Check(result.status == MalformedProtocolRouteStatus,
			QStringLiteral("router rejects malformed input"));

		Check(router.registerHandler(InputProtocolChannel,
			QStringLiteral("badPayload"), {},
			[](const KProtocolEnvelope &, const KProtocolRouteContext &)
			{
				return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed,
					QStringLiteral("payload is invalid"));
			}), QStringLiteral("router registers failing handler"));
		result = router.route(InputProtocolChannel,
			QStringLiteral("{\"version\":1,\"type\":\"badPayload\"}"), context);
		Check(result.status == HandlerFailedProtocolRouteStatus
			&& result.handlerResult.status == ProtocolHandlerDecodeFailed
			&& result.strError == QStringLiteral("payload is invalid"),
			QStringLiteral("handler failure is returned without side-channel state"));
	}

	void TestOutboundQueue()
	{
		KOutboundMessageQueue queue(3, 64);
		KOutboundMessage firstMove { QStringLiteral("move-1"), QStringLiteral("mouseMove"), false };
		KOutboundMessage latestMove { QStringLiteral("move-2"), QStringLiteral("mouseMove"), false };
		KOutboundMessage keyDown { QStringLiteral("key-down"), QString(), true };
		Check(queue.enqueue(firstMove) == EnqueuedOutboundMessage,
			QStringLiteral("queue accepts first move"));
		Check(queue.enqueue(keyDown) == EnqueuedOutboundMessage,
			QStringLiteral("queue accepts reliable input"));
		Check(queue.enqueue(latestMove) == CoalescedOutboundMessage,
			QStringLiteral("queue coalesces mouse move"));

		KOutboundMessage message;
		Check(queue.takeFirst(&message) && message.strPayload == QStringLiteral("key-down"),
			QStringLiteral("queue preserves reliable ordering"));
		Check(queue.takeFirst(&message) && message.strPayload == QStringLiteral("move-2"),
			QStringLiteral("queue places the latest mouse position after earlier input"));

		KOutboundMessageQueue overflowQueue(2, 64);
		overflowQueue.enqueue({ QStringLiteral("move"), QStringLiteral("mouseMove"), false });
		overflowQueue.enqueue({ QStringLiteral("key-1"), QString(), true });
		Check(overflowQueue.enqueue({ QStringLiteral("key-2"), QString(), true })
			== EnqueuedOutboundMessage, QStringLiteral("reliable input evicts coalescible move"));
		Check(overflowQueue.enqueue({ QStringLiteral("key-3"), QString(), true })
			== OverflowOutboundMessage, QStringLiteral("reliable-only overflow is explicit"));

		KOutboundMessageQueue realtimeQueue(1, 32);
		KOutboundMessageQueue reliableQueue(2, 64);
		realtimeQueue.enqueue({ QStringLiteral("move-1"), QStringLiteral("mouseMove"), false });
		Check(realtimeQueue.enqueue({ QStringLiteral("move-2"),
			QStringLiteral("mouseMove"), false }) == CoalescedOutboundMessage,
			QStringLiteral("realtime pointer queue keeps only the newest move"));
		Check(reliableQueue.enqueue({ QStringLiteral("key-down"), QString(), true })
			== EnqueuedOutboundMessage,
			QStringLiteral("realtime pointer pressure does not block reliable input"));
	}

	void TestProtocolEnvelopeValidationAndSingleParse()
	{
		KProtocolEnvelope envelope;
		QString strError;
		Check(!KProtocolEnvelopeCodec::decode(InputProtocolChannel,
			QStringLiteral("{\"type\":\"mouseMove\",\"seq\":\"1\",\"x\":1,\"y\":1}"),
			&envelope, &strError),
			QStringLiteral("missing envelope version is rejected"));
		for (const QString &strSequence : {
			QStringLiteral("-1"), QStringLiteral("1.5"), QStringLiteral("not-a-number"),
			QStringLiteral("18446744073709551616") })
		{
			Check(!KProtocolEnvelopeCodec::decode(InputProtocolChannel,
				QStringLiteral("{\"version\":1,\"type\":\"mouseMove\",\"seq\":\"%1\"}")
					.arg(strSequence), &envelope, &strError),
				QStringLiteral("invalid sequence is rejected: %1").arg(strSequence));
		}
		Check(!KProtocolEnvelopeCodec::decode(InputProtocolChannel,
			QStringLiteral("{\"version\":1,\"type\":\"mouseMove\",\"seq\":1.5}"),
			&envelope, &strError),
			QStringLiteral("fractional numeric sequence is rejected"));
		Check(!KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
			QStringLiteral("{\"version\":1,\"type\":\"endSession\","
				"\"requestId\":\"not-a-uuid\"}"), &envelope, &strError),
			QStringLiteral("invalid request id is rejected"));

		KInputMessage source;
		source.type = MouseMoveInputMessageType;
		source.nSequence = 42;
		source.nX = 10;
		source.nY = 20;
		const QString strEncoded = KInputMessageCodec::encode(source);
		Check(KProtocolEnvelopeCodec::decode(InputProtocolChannel,
			strEncoded, &envelope, &strError),
			QStringLiteral("encoded input has a valid common envelope"));
		envelope.strRawMessage = QStringLiteral("not-json-after-routing");
		envelope.payload.insert(QStringLiteral("futureField"), true);
		KInputMessage decoded;
		Check(KInputMessageCodec::decode(envelope, &decoded, &strError)
			&& decoded.nSequence == 42
			&& decoded.nX == 10
			&& decoded.nY == 20,
			QStringLiteral("typed codec consumes parsed payload and ignores unknown fields"));
	}

	void TestSessionErrorNames()
	{
		KSessionError error;
		error.domain = SignalingSessionErrorDomain;
		error.code = ConnectionTimeoutSessionErrorCode;
		error.stage = ConnectingSessionErrorStage;
		error.bRetryable = true;
		Check(error.isValid(), QStringLiteral("structured error is valid"));
		Check(KSessionError::domainName(error.domain) == QStringLiteral("signaling"),
			QStringLiteral("error domain name is stable"));
		Check(KSessionError::codeName(error.code) == QStringLiteral("connection_timeout"),
			QStringLiteral("error code name is stable"));
		Check(KSessionError::stageName(error.stage) == QStringLiteral("connecting"),
			QStringLiteral("error stage name is stable"));
		Check(KSessionErrorPresenter::userMessage(error).contains(QStringLiteral("超时")),
			QStringLiteral("presentation maps error code to user text"));
		Check(error.bRetryable, QStringLiteral("retry policy is independent from user text"));
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestProtocolRouter();
	TestProtocolEnvelopeValidationAndSingleParse();
	TestOutboundQueue();
	TestSessionErrorNames();
	return g_nFailures == 0 ? 0 : 1;
}
