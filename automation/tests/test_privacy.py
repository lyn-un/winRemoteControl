import os
import time
import unittest
from pathlib import Path

from e2e_support import TwoProcessSession, destructive_e2e_enabled


@unittest.skipUnless(
    destructive_e2e_enabled(),
    "set WRC_RUN_E2E=1 and WRC_RUN_DESTRUCTIVE_AUTOMATION=1 for privacy tests",
)
class PrivacyTests(unittest.TestCase):
    @staticmethod
    def _wait_for_privacy_state(session, expected: str, timeout: float = 15.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = session.get_state().get("privacyModeStatus", {})
            if status.get("state") == expected:
                return status
            time.sleep(0.1)
        raise AssertionError(f"privacy mode did not reach {expected!r}")

    @staticmethod
    def _wait_for_post_session_action(session, expected: str, timeout: float = 15.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = session.get_state().get("postSessionActionStatus", {})
            if status.get("action") == expected and not status.get("errorCode"):
                return status
            time.sleep(0.1)
        raise AssertionError(f"post-session action did not reach {expected!r}")

    def test_privacy_command_uses_persistent_preference_service(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.connect()
            pair.start_streaming()
            pair.controller_session.trigger_command(
                "security.privacy.set", {"mode": "privacyOverlay"}
            )
            self._wait_for_privacy_state(pair.controller_session, "active")
            pair.controller_session.trigger_command(
                "security.privacy.set", {"mode": "disabled"}
            )
            self._wait_for_privacy_state(pair.controller_session, "inactive")
        finally:
            pair.close()

    def test_post_session_action_round_trip_does_not_lock_during_test(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.connect()
            pair.start_streaming()
            pair.controller_session.trigger_command(
                "security.post_session.set", {"action": "lockWorkstation"}
            )
            self._wait_for_post_session_action(
                pair.controller_session, "lockWorkstation"
            )
            pair.controller_session.trigger_command(
                "security.post_session.set", {"action": "none"}
            )
            self._wait_for_post_session_action(pair.controller_session, "none")
        finally:
            pair.close()

    def test_disconnect_restores_controlled_side_privacy_overlay(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.connect()
            pair.start_streaming()
            pair.controller_session.trigger_command(
                "security.privacy.set", {"mode": "privacyOverlay"}
            )
            self._wait_for_privacy_state(pair.controller_session, "active")
            pair.disconnect()
            pair.controller_session.wait_for_state("Idle", timeout=15)
            self._wait_for_privacy_state(pair.host_session, "inactive")
        finally:
            pair.close()


if __name__ == "__main__":
    unittest.main()
