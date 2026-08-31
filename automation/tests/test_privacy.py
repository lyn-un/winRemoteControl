import os
import tempfile
import time
import unittest
from pathlib import Path

from e2e_support import (
    TwoProcessSession,
    destructive_e2e_enabled,
    privacy_e2e_enabled,
)
from wrcdriver import WrcApplication


def _wait_for_privacy_state(session, expected: str, timeout: float = 15.0):
    deadline = time.monotonic() + timeout
    last_state = {}
    while time.monotonic() < deadline:
        last_state = session.get_state()
        status = last_state.get("privacyModeStatus", {})
        if status.get("state") == expected:
            return status
        time.sleep(0.1)
    raise AssertionError(
        f"privacy mode did not reach {expected!r}; state={last_state!r}"
    )


def _wait_for_preference_file(path: Path, remote_device_id: str) -> str:
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        try:
            content = path.read_text(encoding="utf-8")
        except OSError:
            content = ""
        if remote_device_id in content and "privacyOverlay" in content:
            return content
        time.sleep(0.1)
    raise AssertionError(
        f"preference file did not contain device {remote_device_id!r}: {path}"
    )


def _restore_privacy(pair: TwoProcessSession | None) -> None:
    if pair is None or pair.controller_session is None:
        return
    try:
        pair.controller_session.trigger_command(
            "security.privacy.set", {"mode": "disabled"}
        )
        _wait_for_privacy_state(pair.controller_session, "inactive", timeout=5.0)
    except Exception:
        pass


def _cleanup_persistent_identity(executable: Path, profile: Path) -> None:
    application = None
    try:
        application = WrcApplication.launch(
            executable, profile, automation_test_profile=True
        )
    except Exception:
        return
    finally:
        if application is not None:
            application.close()


def _pair_diagnostics(
    pair: TwoProcessSession | None, profile: Path, remote_device_id: str
) -> dict:
    diagnostics = {
        "profile": str(profile),
        "remoteDeviceId": remote_device_id,
        "hostPid": getattr(getattr(pair, "host", None), "pid", None),
        "controllerPid": getattr(getattr(pair, "controller", None), "pid", None),
    }
    if pair is None or pair.controller_session is None:
        return diagnostics
    try:
        state = pair.controller_session.get_state()
        diagnostics.update(
            {
                "generation": state.get("sessionGeneration"),
                "capabilities": state.get("negotiatedCapabilities"),
                "currentError": state.get("currentError"),
                "state": state,
            }
        )
    except Exception as error:
        diagnostics["stateReadError"] = repr(error)
    try:
        cursor = max(0, pair.controller_session.event_cursor - 20)
        diagnostics["recentEvents"] = pair.controller_session.get_events_since(
            cursor
        ).get("events", [])
    except Exception as error:
        diagnostics["eventReadError"] = repr(error)
    return diagnostics


def _assert_preference_uses_device_identity(
    test_case: unittest.TestCase,
    content: str,
    remote_device_id: str,
    state: dict,
) -> None:
    test_case.assertIn(remote_device_id, content)
    test_case.assertNotIn("127.0.0.1", content)
    test_case.assertNotIn("host=", content.lower())
    test_case.assertNotIn("port=", content.lower())
    remote_name = str(state.get("remoteDeviceName", "")).strip()
    if remote_name and remote_name != remote_device_id:
        test_case.assertNotIn(remote_name, content)


@unittest.skipUnless(
    privacy_e2e_enabled(),
    "set WRC_RUN_E2E=1 and WRC_RUN_PRIVACY_E2E=1 for privacy overlay tests",
)
class PrivacyOverlayTests(unittest.TestCase):
    def test_privacy_command_round_trip(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.connect()
            pair.start_streaming()
            pair.controller_session.trigger_command(
                "security.privacy.set", {"mode": "privacyOverlay"}
            )
            _wait_for_privacy_state(pair.controller_session, "active")
            pair.controller_session.trigger_command(
                "security.privacy.set", {"mode": "disabled"}
            )
            _wait_for_privacy_state(pair.controller_session, "inactive")
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
            _wait_for_privacy_state(pair.controller_session, "active")
            pair.disconnect()
            pair.controller_session.wait_for_state("Idle", timeout=15)
            _wait_for_privacy_state(pair.host_session, "inactive")
        finally:
            pair.close()

    def test_privacy_preference_survives_real_process_restart(self):
        executable = Path(os.environ["WRC_EXECUTABLE"]).resolve()
        temporary_parent = executable.parents[1]
        pair = None
        with tempfile.TemporaryDirectory(
            prefix="wrc-privacy-restart-", dir=temporary_parent
        ) as directory:
            profile_root = Path(directory)
            first_pids = ()
            remote_device_id = ""
            try:
                pair = TwoProcessSession(
                    executable,
                    profile_root=profile_root,
                    automation_test_profile=False,
                )
                pair.connect()
                pair.start_streaming()
                first_pids = (pair.host.pid, pair.controller.pid)
                remote_device_id = str(
                    pair.controller_session.get_state().get("remoteDeviceId", "")
                )
                self.assertTrue(remote_device_id)
                pair.controller_session.trigger_command(
                    "security.privacy.set", {"mode": "privacyOverlay"}
                )
                _wait_for_privacy_state(pair.controller_session, "active")
                preference_path = (
                    profile_root / "controller" / "device_security_preferences.ini"
                )
                content = _wait_for_preference_file(
                    preference_path, remote_device_id
                )
                _assert_preference_uses_device_identity(
                    self,
                    content,
                    remote_device_id,
                    pair.controller_session.get_state(),
                )
                pair.close()
                pair = None

                pair = TwoProcessSession(
                    executable,
                    profile_root=profile_root,
                    automation_test_profile=True,
                )
                self.assertNotIn(pair.host.pid, first_pids)
                self.assertNotIn(pair.controller.pid, first_pids)
                pair.connect()
                pair.start_streaming()
                self.assertEqual(
                    pair.controller_session.get_state().get("remoteDeviceId"),
                    remote_device_id,
                )
                _wait_for_privacy_state(pair.controller_session, "active")
                pair.controller_session.trigger_command(
                    "security.privacy.set", {"mode": "disabled"}
                )
                _wait_for_privacy_state(pair.controller_session, "inactive")
            except Exception as error:
                raise AssertionError(
                    "privacy restart E2E failed; diagnostics="
                    f"{_pair_diagnostics(pair, profile_root, remote_device_id)!r}"
                ) from error
            finally:
                _restore_privacy(pair)
                if pair is not None:
                    pair.close()
                _cleanup_persistent_identity(executable, profile_root / "host")
                _cleanup_persistent_identity(executable, profile_root / "controller")

    def test_privacy_preference_isolated_by_remote_device_id(self):
        executable = Path(os.environ["WRC_EXECUTABLE"]).resolve()
        temporary_parent = executable.parents[1]
        pair = None
        with tempfile.TemporaryDirectory(
            prefix="wrc-privacy-device-", dir=temporary_parent
        ) as directory:
            profile_root = Path(directory)
            host_a_profile = profile_root / "host-a"
            remote_device_a = ""
            remote_device_b = ""
            try:
                pair = TwoProcessSession(
                    executable,
                    profile_root=profile_root,
                    automation_test_profile=False,
                )
                pair.connect()
                pair.start_streaming()
                remote_device_a = str(
                    pair.controller_session.get_state().get("remoteDeviceId", "")
                )
                pair.controller_session.trigger_command(
                    "security.privacy.set", {"mode": "privacyOverlay"}
                )
                _wait_for_privacy_state(pair.controller_session, "active")
                preference_path = (
                    profile_root / "controller" / "device_security_preferences.ini"
                )
                content = _wait_for_preference_file(preference_path, remote_device_a)
                _assert_preference_uses_device_identity(
                    self,
                    content,
                    remote_device_a,
                    pair.controller_session.get_state(),
                )
                pair.close()
                pair = None
                (profile_root / "host").rename(host_a_profile)

                pair = TwoProcessSession(
                    executable,
                    profile_root=profile_root,
                    automation_test_profile=True,
                )
                pair.connect()
                pair.start_streaming()
                state = pair.controller_session.get_state()
                remote_device_b = str(state.get("remoteDeviceId", ""))
                self.assertTrue(remote_device_b)
                self.assertNotEqual(remote_device_a, remote_device_b)
                _wait_for_privacy_state(pair.controller_session, "inactive")
                content = preference_path.read_text(encoding="utf-8")
                self.assertIn(remote_device_a, content)
                self.assertNotIn(remote_device_b, content)
            except Exception as error:
                raise AssertionError(
                    "privacy device isolation E2E failed; diagnostics="
                    f"{_pair_diagnostics(pair, profile_root, remote_device_b)!r}; "
                    f"deviceA={remote_device_a!r}"
                ) from error
            finally:
                _restore_privacy(pair)
                if pair is not None:
                    pair.close()
                _cleanup_persistent_identity(executable, host_a_profile)
                _cleanup_persistent_identity(executable, profile_root / "host")
                _cleanup_persistent_identity(executable, profile_root / "controller")


@unittest.skipUnless(
    destructive_e2e_enabled(),
    "set WRC_RUN_E2E=1 and WRC_RUN_DESTRUCTIVE_AUTOMATION=1 for lock tests",
)
class PostSessionActionTests(unittest.TestCase):
    @staticmethod
    def _wait_for_post_session_action(session, expected: str, timeout: float = 15.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = session.get_state().get("postSessionActionStatus", {})
            if status.get("action") == expected and not status.get("errorCode"):
                return status
            time.sleep(0.1)
        raise AssertionError(f"post-session action did not reach {expected!r}")

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


if __name__ == "__main__":
    unittest.main()
