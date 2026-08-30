import time
from typing import Any
from urllib.parse import quote

from .errors import EventHistoryLost, WaitTimeout
from .transport import DriverTransport


class WrcSession:
    def __init__(
        self,
        transport: DriverTransport,
        session_id: str,
        event_cursor: int = 0,
        session_generation: int = 0,
    ) -> None:
        self._transport = transport
        self.session_id = session_id
        self.session_generation = session_generation
        self._last_event_sequence = event_cursor

    @property
    def _base_path(self) -> str:
        return f"/session/{quote(self.session_id, safe='')}"

    @property
    def event_cursor(self) -> int:
        return self._last_event_sequence

    def trigger_command(
        self,
        command_id: str,
        arguments: dict[str, Any] | None = None,
        idempotency_key: str | None = None,
    ) -> Any:
        body = {"id": command_id, "arguments": arguments or {}}
        if idempotency_key is not None:
            body["idempotencyKey"] = idempotency_key
        return self._transport.request(
            "POST",
            f"{self._base_path}/command/trigger",
            body,
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
            requested_cursor = self._last_event_sequence
            snapshot = self.get_events(requested_cursor)
            if snapshot.get("hasGap") is True:
                raise EventHistoryLost(
                    requested_cursor,
                    int(snapshot.get("oldestSequence", 0)),
                    int(snapshot.get("nextSequence", 0)),
                    event_type,
                )
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
