import ctypes
import json
import os
import subprocess
import time
from pathlib import Path

from .errors import DriverNotRunning, ProcessExited, WaitTimeout
from .transport import DriverEndpoint


AUTOMATION_PROTOCOL_VERSION = 1
AUTOMATION_BUILD_ID = "wrc-automation-abi2-20260830"


def discovery_directory() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise DriverNotRunning("LOCALAPPDATA is unavailable")
    return Path(local_app_data) / "winRemoteControl" / "automation"


def _process_is_running(pid: int) -> bool:
    process_query_limited_information = 0x1000
    handle = ctypes.windll.kernel32.OpenProcess(
        process_query_limited_information, False, pid
    )
    if not handle:
        return False
    ctypes.windll.kernel32.CloseHandle(handle)
    return True


def _process_started_at_ms(pid: int) -> int | None:
    process_query_limited_information = 0x1000
    handle = ctypes.windll.kernel32.OpenProcess(
        process_query_limited_information, False, pid
    )
    if not handle:
        return None
    creation = ctypes.c_ulonglong()
    exit_time = ctypes.c_ulonglong()
    kernel = ctypes.c_ulonglong()
    user = ctypes.c_ulonglong()
    try:
        succeeded = ctypes.windll.kernel32.GetProcessTimes(
            handle,
            ctypes.byref(creation),
            ctypes.byref(exit_time),
            ctypes.byref(kernel),
            ctypes.byref(user),
        )
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)
    if not succeeded:
        return None
    return creation.value // 10_000 - 11_644_473_600_000


def load_endpoint(pid: int) -> DriverEndpoint:
    path = discovery_directory() / f"{pid}.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DriverNotRunning(f"Driver discovery file is unavailable for PID {pid}") from error
    if not _process_is_running(pid):
        try:
            path.unlink()
        except OSError:
            pass
        raise ProcessExited(f"Process {pid} is not running")
    try:
        endpoint = DriverEndpoint(
            pid=int(data["pid"]),
            port=int(data["port"]),
            token=str(data["token"]),
            protocol_version=int(data["protocolVersion"]),
            build_id=str(data["buildId"]),
            started_at_ms=int(data["startedAtMs"]),
        )
    except (KeyError, TypeError, ValueError) as error:
        raise DriverNotRunning("Driver discovery file is malformed") from error
    if (
        endpoint.pid != pid
        or endpoint.protocol_version != AUTOMATION_PROTOCOL_VERSION
        or endpoint.build_id != AUTOMATION_BUILD_ID
    ):
        raise DriverNotRunning("Driver discovery metadata does not match the target")
    process_started_at_ms = _process_started_at_ms(pid)
    if (
        process_started_at_ms is None
        or endpoint.started_at_ms < process_started_at_ms
        or endpoint.started_at_ms - process_started_at_ms > 60_000
    ):
        raise DriverNotRunning("Driver discovery file belongs to a different process lifetime")
    return endpoint


def discover_pids() -> list[int]:
    directory = discovery_directory()
    if not directory.exists():
        return []
    pids: list[int] = []
    for path in directory.glob("*.json"):
        try:
            pid = int(path.stem)
            load_endpoint(pid)
            pids.append(pid)
        except (ValueError, DriverNotRunning, ProcessExited):
            continue
    return sorted(pids)


def wait_for_endpoint(pid: int, timeout: float = 15.0) -> DriverEndpoint:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return load_endpoint(pid)
        except DriverNotRunning:
            if not _process_is_running(pid):
                raise ProcessExited(f"Process {pid} exited before the driver started")
            time.sleep(0.1)
    raise WaitTimeout(f"Timed out waiting for automation driver in process {pid}")


def launch_process(
    executable: str | Path,
    profile: str | Path,
    extra_arguments: list[str] | tuple[str, ...] | None = None,
) -> subprocess.Popen[bytes]:
    arguments = [
        str(Path(executable).resolve()),
        "--data-dir",
        str(Path(profile).resolve()),
        "--automation-test-profile",
    ]
    arguments.extend(extra_arguments or ())
    return subprocess.Popen(
        arguments,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def terminate_process(process: subprocess.Popen[bytes], timeout: float = 8.0) -> None:
    if process.poll() is not None:
        return
    user32 = ctypes.windll.user32

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def close_window(window, _):
        process_id = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(window, ctypes.byref(process_id))
        if process_id.value == process.pid:
            user32.PostMessageW(window, 0x0010, 0, 0)
        return True

    user32.EnumWindows(close_window, 0)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)
    discovery_file = discovery_directory() / f"{process.pid}.json"
    try:
        discovery_file.unlink()
    except FileNotFoundError:
        pass
