import hashlib
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from e2e_support import TwoProcessSession, e2e_enabled
from identity_support import cleanup_persistent_identity
from wrcdriver import CommandDisabled, WrcApplication


@unittest.skipUnless(e2e_enabled(), "set WRC_RUN_E2E=1 for native profile lifecycle tests")
class ProfileLifecycleTests(unittest.TestCase):
    def test_persistent_pair_survives_restart_and_clean_disconnect(self):
        executable = Path(os.environ["WRC_EXECUTABLE"]).resolve()
        with tempfile.TemporaryDirectory(prefix="wrc-profile-restart-", dir=executable.parents[1]) as root:
            profiles = Path(root)
            identities = None
            previous_pids = set()
            try:
                for round_index in range(3):
                    pair = TwoProcessSession(
                        executable, profile_root=profiles, automation_test_profile=False,
                    )
                    try:
                        self.assertFalse(previous_pids.intersection({pair.host.pid, pair.controller.pid}))
                        previous_pids.update({pair.host.pid, pair.controller.pid})
                        pair.connect()
                        current = [json.loads((profiles / side / "security" / "device_identity.json")
                                   .read_text(encoding="utf-8")) for side in ("host", "controller")]
                        current = [(item["deviceId"], item["publicKey"]) for item in current]
                        if round_index == 0:
                            identities = current
                        self.assertEqual(identities, current)
                        for side in ("host", "controller"):
                            store = json.loads((profiles / side / "security" / "trusted_devices.json")
                                               .read_text(encoding="utf-8"))
                            self.assertTrue(store["devices"])
                        for session in (pair.host_session, pair.controller_session):
                            self.assertIsNone(session.get_state().get("currentError"))
                        if round_index == 0:
                            pair.disconnect()
                        elif round_index == 1:
                            pair.host_session.trigger_command("session.disconnect")
                            pair._wait_for_disconnected(15)
                        else:
                            with ThreadPoolExecutor(max_workers=2) as executor:
                                pending = [executor.submit(session.trigger_command, "session.disconnect")
                                           for session in (pair.host_session, pair.controller_session)]
                                for future in pending:
                                    try:
                                        future.result()
                                    except CommandDisabled:
                                        # The other peer may have completed teardown before dispatch.
                                        pass
                            pair._wait_for_disconnected(15)
                        for session in (pair.host_session, pair.controller_session):
                            state = session.get_state()
                            for error in (state.get("currentError"), state.get("lastError")):
                                if round_index >= 1 and session is pair.controller_session and error:
                                    self.assertEqual(error["code"], "connection_lost", state)
                                    self.assertEqual(error["technicalMessage"],
                                                     "Remote session ended: remote_local_disconnect", state)
                                else:
                                    self.assertIsNone(error, state)
                    finally:
                        pair.close()
            finally:
                errors = []
                for side in ("host", "controller"):
                    try:
                        cleanup_persistent_identity(executable, profiles / side)
                    except Exception as error:
                        errors.append(str(error))
                if errors:
                    raise AssertionError("Persistent test identity cleanup failed: " + "; ".join(errors))

    def test_temporary_profile_reuse_and_missing_directory_are_rejected(self):
        executable = Path(os.environ["WRC_EXECUTABLE"]).resolve()
        environment = os.environ.copy()
        environment["PATH"] = str(executable.parent) + os.pathsep + environment.get("PATH", "")
        missing = subprocess.run(
            [str(executable), "--automation-test-profile"], env=environment,
            capture_output=True, timeout=15,
        )
        self.assertEqual(missing.returncode, 2)
        with tempfile.TemporaryDirectory(prefix="wrc-profile-once-", dir=executable.parents[1]) as root:
            profile = Path(root)
            application = WrcApplication.launch(executable, profile)
            try:
                session = application.create_session()
                session.trigger_command("application.set_role", {"role": "controlled"})
                session.trigger_command("signaling.start_server", {"port": 0})
            finally:
                application.close()
            files = list((profile / "security").glob("*.json"))
            self.assertTrue(files)
            hashes_before = {file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in files}
            rejected = subprocess.run(
                [str(executable), "--data-dir", str(profile), "--automation-test-profile"],
                env=environment, capture_output=True, timeout=15,
            )
            self.assertEqual(rejected.returncode, 2)
            self.assertEqual(hashes_before, {
                file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in files
            })
            with self.assertRaisesRegex(ValueError, "automation_profile_reuse"):
                WrcApplication.launch(executable, profile)


if __name__ == "__main__":
    unittest.main()
