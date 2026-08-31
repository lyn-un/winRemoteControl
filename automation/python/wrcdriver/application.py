from pathlib import Path
import time

from .errors import DriverNotRunning, DriverProtocolError, ProcessExited, WaitTimeout
from .process import discover_pids, launch_process, terminate_process, wait_for_endpoint
from .session import WrcSession
from .transport import DriverTransport


class WrcApplication:
    def __init__(self, transport: DriverTransport, process=None) -> None:
        self._transport = transport
        self._process = process
        self._sessions: list[WrcSession] = []

    @classmethod
    def attach(cls, pid: int | None = None) -> "WrcApplication":
        if pid is None:
            pids = discover_pids()
            if len(pids) != 1:
                raise DriverNotRunning(
                    f"Expected one running driver, found {len(pids)}; specify pid"
                )
            pid = pids[0]
        application = cls(DriverTransport(wait_for_endpoint(pid)))
        application.wait_until_ready()
        return application

    @classmethod
    def launch(
        cls,
        executable: str | Path,
        profile: str | Path,
        role: str | None = None,
        extra_arguments: list[str] | tuple[str, ...] | None = None,
        automation_test_profile: bool = True,
    ) -> "WrcApplication":
        process = launch_process(
            executable,
            profile,
            extra_arguments,
            automation_test_profile=automation_test_profile,
        )
        try:
            application = cls(DriverTransport(wait_for_endpoint(process.pid)), process)
            application.wait_until_ready()
            if role is not None:
                session = application.create_session()
                session.trigger_command("application.set_role", {"role": role})
            return application
        except Exception:
            terminate_process(process)
            raise

    @property
    def pid(self) -> int:
        return self._transport.endpoint.pid

    def status(self):
        return self._transport.request("GET", "/status")

    def wait_until_ready(self, timeout: float = 15.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._process is not None and self._process.poll() is not None:
                raise ProcessExited(
                    f"Process {self._process.pid} exited before the application was ready"
                )
            status = self.status()
            if not isinstance(status, dict):
                raise DriverProtocolError("Driver status response shape is invalid")
            if status.get("ready") is True:
                return status
            time.sleep(0.05)
        raise WaitTimeout("Timed out waiting for the application host to become ready")

    def create_session(self) -> WrcSession:
        value = self._transport.request("POST", "/session", {})
        if not isinstance(value, dict) or not value.get("sessionId"):
            raise DriverNotRunning("Driver did not return a session id")
        session = WrcSession(
            self._transport,
            str(value["sessionId"]),
            int(value.get("eventCursor", 0)),
            int(value.get("sessionGeneration", 0)),
        )
        self._sessions.append(session)
        return session

    def close(self) -> None:
        for session in reversed(self._sessions):
            try:
                session.quit()
            except Exception:
                pass
        self._sessions.clear()
        if self._process is not None:
            terminate_process(self._process)
            self._process = None

    def __enter__(self) -> "WrcApplication":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()
