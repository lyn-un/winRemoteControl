import os
import unittest
from pathlib import Path

from e2e_support import TwoProcessSession, e2e_enabled


@unittest.skipUnless(e2e_enabled(), "set WRC_RUN_E2E=1 to run two-process tests")
class StreamingTests(unittest.TestCase):
    def test_remote_frames_continue_arriving(self):
        pair = TwoProcessSession(Path(os.environ["WRC_EXECUTABLE"]))
        try:
            pair.connect()
            state = pair.start_streaming()
            self.assertGreater(int(state["receivedFrameCount"]), 0)
            pair.controller_session.trigger_command("stream.stop")
            pair.controller_session.wait_for_state("Connected", timeout=15)
        finally:
            pair.close()


if __name__ == "__main__":
    unittest.main()
