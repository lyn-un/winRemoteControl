import http.client
import json
from dataclasses import dataclass
from typing import Any

from .errors import (
    CommandDisabled,
    CommandBusy,
    CommandTimeout,
    DriverNotRunning,
    DriverProtocolError,
    InternalError,
    InvalidArgument,
    InvalidSessionId,
    UnknownCommand,
    UnsupportedOperation,
)


@dataclass(frozen=True)
class DriverEndpoint:
    pid: int
    port: int
    token: str
    protocol_version: int
    build_id: str
    started_at_ms: int


class DriverTransport:
    def __init__(self, endpoint: DriverEndpoint, timeout: float = 10.0) -> None:
        self.endpoint = endpoint
        self.timeout = timeout

    def request(
        self, method: str, path: str, body: dict[str, Any] | None = None
    ) -> Any:
        encoded = None
        headers = {"X-WRC-Token": self.endpoint.token, "Accept": "application/json"}
        if body is not None:
            encoded = json.dumps(body, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json; charset=utf-8"
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.endpoint.port, timeout=self.timeout
        )
        try:
            connection.request(method, path, body=encoded, headers=headers)
            response = connection.getresponse()
            raw = response.read()
        except (OSError, TimeoutError, http.client.HTTPException) as error:
            raise DriverNotRunning(str(error)) from error
        finally:
            connection.close()
        try:
            payload = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise DriverProtocolError("Driver returned invalid JSON") from error
        if not isinstance(payload, dict) or "isSuccess" not in payload:
            raise DriverProtocolError("Driver response shape is invalid")
        if payload.get("isSuccess"):
            return payload.get("value")
        value = payload.get("value")
        if not isinstance(value, dict):
            raise DriverProtocolError("Driver error response shape is invalid")
        error_code = str(value.get("error", "internal_error"))
        message = str(value.get("message", error_code))
        exception_type = {
            "invalid_argument": InvalidArgument,
            "invalid_session_id": InvalidSessionId,
            "unknown_command": UnknownCommand,
            "command_disabled": CommandDisabled,
            "command_busy": CommandBusy,
            "command_timeout": CommandTimeout,
            "unsupported_operation": UnsupportedOperation,
            "internal_error": InternalError,
        }.get(error_code, DriverProtocolError)
        raise exception_type(message)
