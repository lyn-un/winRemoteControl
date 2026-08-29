import os
import unittest
from pathlib import Path

from e2e_support import TwoProcessSession, e2e_enabled


@unittest.skipUnless(e2e_enabled(), "set WRC_RUN_E2E=1 to run two-process tests")
class DisconnectTests(unittest.TestCase):
    def test_controller_disconnects_cleanly(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.connect()
            pair.disconnect()
            pair.controller_session.wait_for_state("Idle", timeout=15)
        finally:
            pair.close()


if __name__ == "__main__":
    unittest.main()
