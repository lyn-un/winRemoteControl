import os
import socket
import time
import unittest
from pathlib import Path

from e2e_support import TwoProcessSession, e2e_enabled


@unittest.skipUnless(e2e_enabled(), "set WRC_RUN_E2E=1 to run two-process tests")
class ConnectionTests(unittest.TestCase):
    def test_two_processes_connect_through_normal_approval(self):
        executable = Path(os.environ["WRC_EXECUTABLE"])
        pair = TwoProcessSession(executable)
        try:
            pair.connect()
        finally:
            pair.close()

    def test_access_rejection_has_a_stable_reason(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.reject_access()
            pair.wait_for_error("approval_rejected", timeout=15)
        finally:
            pair.close()

    def test_connection_failure_has_a_stable_reason(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.controller_session.trigger_command(
                "application.set_role", {"role": "controller"}
            )
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                probe.bind(("127.0.0.1", 0))
                unused_port = probe.getsockname()[1]
            pair.controller_session.trigger_command(
                "session.connect", {"host": "127.0.0.1", "port": unused_port}
            )
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                state = pair.controller_session.get_state()
                if state.get("lastError"):
                    break
                time.sleep(0.1)
            else:
                self.fail("connection failure did not expose a stable error")
        finally:
            pair.close()

    def test_insufficient_permission_is_reported(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.connect(permissions=["viewScreen"])
            pair.start_streaming()
            pair.controller_session.trigger_command(
                "security.privacy.set", {"mode": "privacyOverlay"}
            )
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                status = pair.controller_session.get_state().get(
                    "privacyModeStatus", {}
                )
                if status.get("errorCode") == "permission_denied":
                    break
                time.sleep(0.1)
            else:
                self.fail("privacy command did not report permission_denied")
        finally:
            pair.close()

    def test_unanswered_pairing_times_out_with_a_stable_reason(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.begin_connection()
            pair.wait_for_error("authentication_timeout", timeout=45)
        finally:
            pair.close()


if __name__ == "__main__":
    unittest.main()
