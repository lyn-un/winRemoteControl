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

from wrcdriver.application import WrcApplication
from wrcdriver.errors import (
    CommandBusy,
    CommandDisabled,
    CommandTimeout,
    DriverNotRunning,
    DriverProtocolError,
    InvalidArgument,
    ProcessExited,
    WaitTimeout,
)
from wrcdriver import process as process_module
from wrcdriver import __main__ as cli_module
from wrcdriver.session import WrcSession
from wrcdriver.transport import DriverEndpoint, DriverTransport


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
        return {"events": [event for event in events if int(event["sequence"]) > since]}


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
        with self.assertRaises(CommandDisabled):
            session.trigger_command("disabled.command")
        with self.assertRaises(CommandBusy):
            session.trigger_command("busy.command")
        with self.assertRaises(InvalidArgument):
            session.trigger_command("invalid.command")
        with self.assertRaises(CommandTimeout):
            session.trigger_command("timeout.command")

    def test_wait_for_state_is_bounded(self):
        session = WrcSession(_FakeTransport(), "session-id")
        with self.assertRaises(WaitTimeout):
            session.wait_for_state("Streaming", timeout=0.01)

    def test_wait_for_event_does_not_return_an_old_event_twice(self):
        session = WrcSession(_FakeEventTransport(), "session-id")
        self.assertEqual(session.wait_for_event("first", timeout=0.1)["sequence"], "1")
        self.assertEqual(session.wait_for_event("second", timeout=0.1)["sequence"], "2")

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
                "buildId": "test",
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
