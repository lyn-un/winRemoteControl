import json
import io
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock
from contextlib import redirect_stdout

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from wrcdriver.application import WrcApplication
from wrcdriver.errors import (
    ApplicationNotReady,
    ApplicationShuttingDown,
    CommandBusy,
    CommandDisabled,
    CommandExecutionStarted,
    CommandTimeout,
    DriverNotRunning,
    DriverProtocolError,
    EventHistoryLost,
    InvalidArgument,
    StaleSessionGeneration,
    ProcessExited,
    WaitTimeout,
)
from wrcdriver import process as process_module
from wrcdriver.process import AUTOMATION_BUILD_ID
from wrcdriver import __main__ as cli_module
from wrcdriver.session import WrcSession
from wrcdriver.transport import DriverEndpoint, DriverTransport
from e2e_support import wait_for_frame_progress, wait_for_stream_stopped


class _DriverHandler(BaseHTTPRequestHandler):
    token = "test-token"

    def do_GET(self):
        if self.headers.get("X-WRC-Token") != self.token:
            self.send_response(401)
            self.end_headers()
            return
        if self.path == "/invalid-json":
            payload = b"not-json"
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        if self.path == "/closed-connection":
            self.close_connection = True
            return
        self._write({"status": 0, "value": {"sessionState": "Connected"}, "isSuccess": True})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length).decode("utf-8"))
        command_errors = {
            "disabled.command": (4, "command_disabled"),
            "busy.command": (5, "command_busy"),
            "invalid.command": (1, "invalid_argument"),
            "timeout.command": (6, "command_timeout"),
            "started.command": (7, "command_execution_started"),
            "stale.command": (8, "stale_generation"),
            "not-ready.command": (8, "application_not_ready"),
            "shutdown.command": (8, "application_shutdown"),
        }
        if body.get("id") in command_errors:
            status, error = command_errors[body["id"]]
            self._write(
                {
                    "status": status,
                    "value": {
                        "error": error,
                        "message": error,
                        "stacktrace": "",
                        "requestId": "request-7",
                        "retryable": False,
                        "outcomeUnknown": error == "command_execution_started",
                    },
                    "isSuccess": False,
                }
            )
            return
        self._write({"status": 0, "value": body, "isSuccess": True})

    def _write(self, value):
        payload = json.dumps(value).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        return


class _FakeTransport:
    def __init__(self):
        self.calls = 0

    def request(self, method, path, body=None):
        self.calls += 1
        return {"sessionState": "Idle"}


class _FakeEventTransport:
    def request(self, method, path, body=None):
        since = int(path.rsplit("=", 1)[-1])
        events = [
            {"sequence": "1", "type": "first"},
            {"sequence": "2", "type": "second"},
        ]
        return {
            "events": [event for event in events if int(event["sequence"]) > since],
            "hasGap": False,
        }


class _FakeGapTransport:
    def request(self, method, path, body=None):
        return {
            "events": [],
            "hasGap": True,
            "oldestSequence": "20",
            "nextSequence": "31",
        }


class _FakeReadyTransport:
    def __init__(self):
        self.calls = 0

    def request(self, method, path, body=None):
        self.calls += 1
        return {
            "driverReady": True,
            "hostReady": self.calls >= 3,
            "ready": self.calls >= 3,
        }


class _FakeClock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class _DelayedLaunchTransport:
    def __init__(self, clock):
        self.clock = clock
        self.calls = []

    def request(self, method, path, body=None):
        self.calls.append((self.clock.now, method, path, body))
        if path == "/status":
            return {"ready": self.clock.now >= 5.1}
        if path == "/session":
            return {
                "sessionId": "delayed-session",
                "eventCursor": "0",
                "sessionGeneration": "1",
            }
        return {}


class _FakeFrameSession:
    def __init__(self, states):
        self._states = list(states)

    def get_state(self):
        if len(self._states) > 1:
            return self._states.pop(0)
        return self._states[0]


class PythonSdkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), _DriverHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def transport(self):
        endpoint = DriverEndpoint(
            pid=1,
            port=self.server.server_port,
            token="test-token",
            protocol_version=1,
            build_id="test",
            started_at_ms=1,
        )
        return DriverTransport(endpoint, timeout=2)

    def test_transport_success_and_error_mapping(self):
        session = WrcSession(self.transport(), "session-id")
        result = session.trigger_command("session.disconnect", {"reason": "test"})
        self.assertEqual(result["id"], "session.disconnect")
        stable_errors = (
            ("disabled.command", CommandDisabled, "command_disabled", 4),
            ("busy.command", CommandBusy, "command_busy", 5),
            ("invalid.command", InvalidArgument, "invalid_argument", 1),
            ("timeout.command", CommandTimeout, "command_timeout", 6),
            ("stale.command", StaleSessionGeneration, "stale_generation", 8),
            ("not-ready.command", ApplicationNotReady, "application_not_ready", 8),
            ("shutdown.command", ApplicationShuttingDown, "application_shutdown", 8),
        )
        for command_id, exception_type, error_code, driver_status in stable_errors:
            with self.subTest(command_id=command_id):
                with self.assertRaises(exception_type) as context:
                    session.trigger_command(command_id)
                self.assertEqual(context.exception.error_code, error_code)
                self.assertEqual(context.exception.driver_status, driver_status)
                self.assertEqual(context.exception.request_id, "request-7")
                self.assertFalse(context.exception.retryable)
        with self.assertRaises(CommandExecutionStarted) as context:
            session.trigger_command("started.command")
        self.assertEqual(context.exception.error_code, "command_execution_started")
        self.assertEqual(context.exception.driver_status, 7)
        self.assertFalse(context.exception.retryable)
        self.assertTrue(context.exception.outcome_unknown)
        self.assertEqual(context.exception.request_id, "request-7")

    def test_trigger_command_sends_optional_idempotency_key(self):
        session = WrcSession(self.transport(), "session-id")
        result = session.trigger_command(
            "session.disconnect", {"reason": "test"}, idempotency_key="operation-1"
        )
        self.assertEqual(result["idempotencyKey"], "operation-1")

    def test_application_waits_until_host_is_ready(self):
        transport = _FakeReadyTransport()
        application = WrcApplication(transport)
        status = application.wait_until_ready(timeout=1)
        self.assertTrue(status["ready"])
        self.assertEqual(transport.calls, 3)

    def test_launch_waits_more_than_five_seconds_before_first_command(self):
        clock = _FakeClock()
        transport = _DelayedLaunchTransport(clock)
        process = mock.Mock(pid=4321)
        process.poll.return_value = None
        with mock.patch(
            "wrcdriver.application.launch_process", return_value=process
        ), mock.patch(
            "wrcdriver.application.wait_for_endpoint", return_value=mock.Mock()
        ), mock.patch(
            "wrcdriver.application.DriverTransport", return_value=transport
        ), mock.patch(
            "wrcdriver.application.time.monotonic", side_effect=clock.monotonic
        ), mock.patch(
            "wrcdriver.application.time.sleep", side_effect=clock.sleep
        ):
            application = WrcApplication.launch(
                "winRemoteControl.exe", "profile", role="controller"
            )
        first_non_status = next(call for call in transport.calls if call[2] != "/status")
        self.assertGreaterEqual(first_non_status[0], 5.1)
        application._process = None

    def test_wait_for_state_is_bounded(self):
        session = WrcSession(_FakeTransport(), "session-id")
        with self.assertRaises(WaitTimeout):
            session.wait_for_state("Streaming", timeout=0.01)

    def test_wait_for_event_does_not_return_an_old_event_twice(self):
        session = WrcSession(_FakeEventTransport(), "session-id")
        self.assertEqual(session.wait_for_event("first", timeout=0.1)["sequence"], "1")
        self.assertEqual(session.wait_for_event("second", timeout=0.1)["sequence"], "2")

    def test_wait_for_event_starts_at_session_baseline(self):
        session = WrcSession(_FakeEventTransport(), "session-id", event_cursor=1)
        self.assertEqual(session.wait_for_event("second", timeout=0.1)["sequence"], "2")

    def test_wait_for_event_reports_history_gap(self):
        session = WrcSession(_FakeGapTransport(), "session-id", event_cursor=10)
        with self.assertRaises(EventHistoryLost) as context:
            session.wait_for_event("pairing.requested", timeout=0.1)
        self.assertEqual(context.exception.requested_cursor, 10)
        self.assertEqual(context.exception.oldest_sequence, 20)
        self.assertEqual(context.exception.next_sequence, 31)

    def test_frame_progress_requires_two_distinct_new_samples(self):
        now_ms = int(__import__("time").time() * 1000)
        baseline = {
            "receivedFrameCount": "10",
            "lastFrameTimestampMs": str(now_ms - 20),
            "sessionGeneration": "3",
        }
        common = {
            "sessionState": "Streaming",
            "webRtcState": "connected",
            "sessionChannelOpen": True,
            "sessionGeneration": "3",
            "lastFrameWidth": 1280,
            "lastFrameHeight": 720,
            "currentError": None,
        }
        one_frame = dict(
            common, receivedFrameCount="11", lastFrameTimestampMs=str(now_ms)
        )
        old_frame = dict(
            common,
            receivedFrameCount="10",
            lastFrameTimestampMs=str(now_ms - 20),
        )
        with self.assertRaises(WaitTimeout):
            wait_for_frame_progress(
                _FakeFrameSession([old_frame]), baseline,
                observation_seconds=0, timeout=0.01,
            )
        with self.assertRaises(WaitTimeout):
            wait_for_frame_progress(
                _FakeFrameSession([one_frame]), baseline,
                observation_seconds=0, timeout=0.01,
            )
        second_frame = dict(
            common, receivedFrameCount="12", lastFrameTimestampMs=str(now_ms + 1)
        )
        result = wait_for_frame_progress(
            _FakeFrameSession([one_frame, second_frame]), baseline,
            observation_seconds=0, timeout=0.5,
        )
        self.assertEqual(result["receivedFrameCount"], "12")

    def test_stream_stop_requires_a_quiet_connected_period(self):
        connected = {
            "sessionState": "Connected",
            "currentError": None,
            "receivedFrameCount": "20",
            "lastFrameTimestampMs": "100",
        }
        result = wait_for_stream_stopped(
            _FakeFrameSession([connected, connected]), quiet_seconds=0, timeout=0.5
        )
        self.assertEqual(result["sessionState"], "Connected")
        growing = [
            dict(connected, receivedFrameCount=str(count), lastFrameTimestampMs=str(count))
            for count in range(20, 40)
        ]
        with self.assertRaises(WaitTimeout):
            wait_for_stream_stopped(
                _FakeFrameSession(growing), quiet_seconds=1, timeout=0.01
            )

    def test_transport_rejects_invalid_json_and_closed_connection(self):
        transport = self.transport()
        with self.assertRaises(DriverProtocolError):
            transport.request("GET", "/invalid-json")
        with self.assertRaises(DriverNotRunning):
            transport.request("GET", "/closed-connection")

    def test_attach_requires_pid_when_multiple_drivers_exist(self):
        with mock.patch("wrcdriver.application.discover_pids", return_value=[11, 22]):
            with self.assertRaises(DriverNotRunning):
                WrcApplication.attach()

    def test_load_endpoint_rejects_stale_discovery_and_exited_process(self):
        with tempfile.TemporaryDirectory() as directory:
            discovery = Path(directory)
            data = {
                "pid": 123,
                "port": 4567,
                "token": "token",
                "protocolVersion": 1,
                "buildId": AUTOMATION_BUILD_ID,
                "startedAtMs": 1_000,
            }
            path = discovery / "123.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with mock.patch.object(process_module, "discovery_directory", return_value=discovery), mock.patch.object(
                process_module, "_process_is_running", return_value=True
            ), mock.patch.object(process_module, "_process_started_at_ms", return_value=2_000):
                with self.assertRaises(DriverNotRunning):
                    process_module.load_endpoint(123)

            path.write_text(json.dumps(data), encoding="utf-8")
            with mock.patch.object(process_module, "discovery_directory", return_value=discovery), mock.patch.object(
                process_module, "_process_is_running", return_value=False
            ):
                with self.assertRaises(ProcessExited):
                    process_module.load_endpoint(123)
                self.assertFalse(path.exists())

    def test_load_endpoint_rejects_a_different_build_id(self):
        with tempfile.TemporaryDirectory() as directory:
            discovery = Path(directory)
            path = discovery / "123.json"
            path.write_text(
                json.dumps(
                    {
                        "pid": 123,
                        "port": 4567,
                        "token": "token",
                        "protocolVersion": 1,
                        "buildId": "old-automation-build",
                        "startedAtMs": 2_000,
                    }
                ),
                encoding="utf-8",
            )
            with mock.patch.object(
                process_module, "discovery_directory", return_value=discovery
            ), mock.patch.object(
                process_module, "_process_is_running", return_value=True
            ), mock.patch.object(
                process_module, "_process_started_at_ms", return_value=2_000
            ):
                with self.assertRaises(DriverNotRunning):
                    process_module.load_endpoint(123)

    def test_launch_cleans_up_when_driver_does_not_start(self):
        fake_process = mock.Mock(pid=321)
        with mock.patch(
            "wrcdriver.application.launch_process", return_value=fake_process
        ), mock.patch(
            "wrcdriver.application.wait_for_endpoint",
            side_effect=WaitTimeout("driver timeout"),
        ), mock.patch("wrcdriver.application.terminate_process") as terminate:
            with self.assertRaises(WaitTimeout):
                WrcApplication.launch("missing.exe", "profile")
            terminate.assert_called_once_with(fake_process)

    def test_cli_closes_attached_application(self):
        application = mock.Mock()
        application.status.return_value = {"ready": True}
        with mock.patch.object(sys, "argv", ["wrcdriver", "--pid", "123", "status"]), mock.patch.object(
            cli_module.WrcApplication, "attach", return_value=application
        ), redirect_stdout(io.StringIO()):
            self.assertEqual(cli_module.main(), 0)
        application.close.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
