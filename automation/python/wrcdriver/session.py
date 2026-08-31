import base64
import time
from typing import Any
from urllib.parse import quote

from .errors import DriverProtocolError, EventHistoryLost, WaitTimeout
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

    def get_supported_commands(self) -> list[str]:
        commands = self.get_state().get("supportedCommands", [])
        if not isinstance(commands, list) or not all(
            isinstance(command, str) for command in commands
        ):
            raise DriverProtocolError("Automation supported command list is malformed")
        return commands

    def send_terminal_input(
        self,
        data: bytes | str,
        idempotency_key: str | None = None,
    ) -> Any:
        encoded = data.encode("utf-8") if isinstance(data, str) else bytes(data)
        if not encoded:
            raise ValueError("terminal input must not be empty")
        return self.trigger_command(
            "terminal.input",
            {"dataBase64": base64.b64encode(encoded).decode("ascii")},
            idempotency_key=idempotency_key,
        )

    def get_events_since(self, sequence: int) -> dict[str, Any]:
        if isinstance(sequence, bool) or not isinstance(sequence, int) or sequence < 0:
            raise ValueError("sequence must be a non-negative integer")
        value = self._transport.request(
            "GET", f"{self._base_path}/events?sinceSequence={sequence}"
        )
        return value if isinstance(value, dict) else {}

    def poll_events(self) -> dict[str, Any]:
        requested_cursor = self._last_event_sequence
        snapshot = self.get_events_since(requested_cursor)
        if snapshot.get("hasGap") is True:
            raise EventHistoryLost(
                requested_cursor,
                int(snapshot.get("oldestSequence", 0)),
                int(snapshot.get("nextSequence", 0)),
                self.session_generation,
            )
        events = snapshot.get("events", [])
        if not isinstance(events, list):
            raise DriverProtocolError("Automation events response is malformed")
        maximum_sequence = requested_cursor
        for event in events:
            if not isinstance(event, dict):
                raise DriverProtocolError("Automation event is malformed")
            maximum_sequence = max(maximum_sequence, int(event.get("sequence", 0)))
        next_sequence = int(snapshot.get("nextSequence", maximum_sequence + 1))
        if next_sequence < 1:
            raise DriverProtocolError("Automation next event sequence is invalid")
        self._last_event_sequence = max(
            requested_cursor, maximum_sequence, next_sequence - 1
        )
        return snapshot

    def get_events(self, since_sequence: int | None = None) -> dict[str, Any]:
        if since_sequence is None:
            return self.poll_events()
        return self.get_events_since(since_sequence)

    def reset_event_cursor(self, sequence: int) -> None:
        if isinstance(sequence, bool) or not isinstance(sequence, int) or sequence < 0:
            raise ValueError("sequence must be a non-negative integer")
        self._last_event_sequence = sequence

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
            snapshot = self.poll_events()
            for event in snapshot.get("events", []):
                if event.get("type") == event_type:
                    return event
            time.sleep(0.1)
        raise WaitTimeout(f"Timed out waiting for event {event_type!r}")

    def quit(self) -> None:
        self._transport.request("DELETE", self._base_path)
