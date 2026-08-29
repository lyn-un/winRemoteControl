import time
from typing import Any
from urllib.parse import quote

from .errors import WaitTimeout
from .transport import DriverTransport


class WrcSession:
    def __init__(self, transport: DriverTransport, session_id: str) -> None:
        self._transport = transport
        self.session_id = session_id
        self._last_event_sequence = 0

    @property
    def _base_path(self) -> str:
        return f"/session/{quote(self.session_id, safe='')}"

    def trigger_command(
        self, command_id: str, arguments: dict[str, Any] | None = None
    ) -> Any:
        return self._transport.request(
            "POST",
            f"{self._base_path}/command/trigger",
            {"id": command_id, "arguments": arguments or {}},
        )

    def get_state(self) -> dict[str, Any]:
        value = self._transport.request("GET", f"{self._base_path}/state")
        return value if isinstance(value, dict) else {}

    def get_events(self, since_sequence: int = 0) -> dict[str, Any]:
        value = self._transport.request(
            "GET", f"{self._base_path}/events?sinceSequence={since_sequence}"
        )
        return value if isinstance(value, dict) else {}

    def wait_for_state(self, expected: str, timeout: float = 30.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last_state: dict[str, Any] = {}
        while time.monotonic() < deadline:
            last_state = self.get_state()
            if last_state.get("sessionState") == expected:
                return last_state
            time.sleep(0.1)
        raise WaitTimeout(
            f"Timed out waiting for session state {expected!r}; last state={last_state!r}"
        )

    def wait_for_event(self, event_type: str, timeout: float = 30.0) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            snapshot = self.get_events(self._last_event_sequence)
            for event in snapshot.get("events", []):
                self._last_event_sequence = max(
                    self._last_event_sequence, int(event.get("sequence", 0))
                )
                if event.get("type") == event_type:
                    return event
            time.sleep(0.1)
        raise WaitTimeout(f"Timed out waiting for event {event_type!r}")

    def quit(self) -> None:
        self._transport.request("DELETE", self._base_path)
