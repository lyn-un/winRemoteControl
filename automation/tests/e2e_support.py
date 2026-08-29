import os
import tempfile
import time
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from wrcdriver import WaitTimeout, WrcApplication


PERMISSIONS = [
    "viewScreen",
    "inputControl",
    "clipboard",
    "terminal",
    "fileTransfer",
]


class TwoProcessSession:
    def __init__(self, executable: str | Path) -> None:
        build_directory = Path(executable).resolve().parents[1]
        self._temporary = tempfile.TemporaryDirectory(
            prefix="wrc-automation-", dir=build_directory
        )
        root = Path(self._temporary.name)
        self.host = None
        self.controller = None
        self.host_session = None
        self.controller_session = None
        try:
            self.host = WrcApplication.launch(executable, root / "host")
            self.controller = WrcApplication.launch(executable, root / "controller")
            self.host_session = self.host.create_session()
            self.controller_session = self.controller.create_session()
        except Exception:
            self.close()
            raise
        self._host_sequence = 0
        self._controller_sequence = 0

    def begin_connection(self, timeout: float = 30.0) -> None:
        self.host_session.trigger_command(
            "application.set_role", {"role": "controlled"}
        )
        self.host_session.trigger_command("signaling.start_server", {"port": 0})
        port = self._wait_for_listening_port(timeout)
        self.controller_session.trigger_command(
            "application.set_role", {"role": "controller"}
        )
        self.controller_session.trigger_command(
            "session.connect", {"host": "127.0.0.1", "port": port}
        )

    def connect(
        self, timeout: float = 30.0, permissions: list[str] | None = None
    ) -> None:
        self.begin_connection(timeout)
        self._complete_pairing_and_access(timeout, permissions or PERMISSIONS, True)
        self._wait_for_connected(timeout)

    def reject_access(self, timeout: float = 30.0) -> None:
        self.begin_connection(timeout)
        self._complete_pairing_and_access(timeout, PERMISSIONS, False)

    def wait_for_error(self, expected: str, timeout: float = 30.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.controller_session.get_state()
            if state.get("lastError") == expected:
                return state
            time.sleep(0.1)
        raise WaitTimeout(f"Timed out waiting for error {expected!r}")

    def start_streaming(self, timeout: float = 30.0) -> dict:
        self.controller_session.trigger_command("stream.start")
        deadline = time.monotonic() + timeout
        first_count = 0
        while time.monotonic() < deadline:
            state = self.controller_session.get_state()
            count = int(state.get("receivedFrameCount", "0"))
            last_frame_timestamp_ms = int(state.get("lastFrameTimestampMs", "0"))
            if (
                state.get("sessionState") == "Streaming"
                and state.get("webRtcState") == "connected"
                and state.get("sessionChannelOpen") is True
                and count > first_count
                and abs(int(time.time() * 1000) - last_frame_timestamp_ms) < 5_000
                and not state.get("lastError")
            ):
                return state
            first_count = max(first_count, count)
            time.sleep(0.1)
        raise WaitTimeout("Timed out waiting for verified streaming frames")

    def disconnect(self) -> None:
        if self.controller_session is None:
            return
        try:
            self.controller_session.trigger_command("session.disconnect")
        except Exception:
            pass

    def close(self) -> None:
        self.disconnect()
        if self.controller is not None:
            self.controller.close()
            self.controller = None
        if self.host is not None:
            self.host.close()
            self.host = None
        self._temporary.cleanup()

    def _wait_for_listening_port(self, timeout: float) -> int:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.host_session.get_state()
            port = int(state.get("listeningPort", 0))
            if state.get("listeningAvailable") and port > 0:
                return port
            time.sleep(0.1)
        raise WaitTimeout("Timed out waiting for controlled-side listener")

    def _events(self, session, sequence_attribute: str):
        sequence = getattr(self, sequence_attribute)
        snapshot = session.get_events(sequence)
        events = snapshot.get("events", [])
        for event in events:
            sequence = max(sequence, int(event.get("sequence", 0)))
        setattr(self, sequence_attribute, sequence)
        return events

    def _complete_pairing_and_access(
        self, timeout: float, permissions: list[str], access_accepted: bool
    ) -> None:
        deadline = time.monotonic() + timeout
        pairing_events = {}
        responded_pairing = set()
        access_responded = False
        while time.monotonic() < deadline:
            for side, session, sequence_attribute in (
                ("host", self.host_session, "_host_sequence"),
                ("controller", self.controller_session, "_controller_sequence"),
            ):
                for event in self._events(session, sequence_attribute):
                    if event.get("type") == "pairing.requested":
                        pairing_events[side] = event
                    elif event.get("type") == "access.requested" and side == "host":
                        session.trigger_command(
                            "access.respond",
                            {
                                "requestId": event["requestId"],
                                "accepted": access_accepted,
                            },
                        )
                        access_responded = True
                        if not access_accepted:
                            return
            if len(pairing_events) == 2 and not responded_pairing:
                if (
                    pairing_events["host"].get("verificationCode")
                    != pairing_events["controller"].get("verificationCode")
                ):
                    raise AssertionError("TLS pairing verification codes do not match")
                for side, session in (
                    ("host", self.host_session),
                    ("controller", self.controller_session),
                ):
                    event = pairing_events[side]
                    session.trigger_command(
                        "pairing.respond",
                        {
                            "requestId": event["requestId"],
                            "accepted": True,
                            "permissions": permissions,
                        },
                    )
                    responded_pairing.add(side)
            host_state = self.host_session.get_state().get("sessionState")
            controller_state = self.controller_session.get_state().get("sessionState")
            if host_state in {"Connected", "Streaming"} and controller_state in {
                "Connected",
                "Streaming",
            }:
                return
            if not pairing_events and access_responded:
                continue
            time.sleep(0.1)
        raise WaitTimeout("Timed out completing normal pairing and access approval")

    def _wait_for_connected(self, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            host = self.host_session.get_state()
            controller = self.controller_session.get_state()
            if (
                host.get("sessionState") == "Connected"
                and controller.get("sessionState") == "Connected"
                and controller.get("webRtcState") == "connected"
                and controller.get("sessionChannelOpen") is True
            ):
                return
            time.sleep(0.1)
        raise WaitTimeout("Timed out waiting for both processes to connect")


def e2e_enabled() -> bool:
    return os.environ.get("WRC_RUN_E2E") == "1"


def destructive_e2e_enabled() -> bool:
    return e2e_enabled() and os.environ.get("WRC_RUN_DESTRUCTIVE_AUTOMATION") == "1"
