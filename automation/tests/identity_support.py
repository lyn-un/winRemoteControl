"""Cleanup for persistent identities created by isolated automation fixtures."""

import json
import os
from pathlib import Path
import subprocess
import uuid


def cleanup_persistent_identity(executable: Path, profile: Path) -> None:
    security = profile / "security"
    descriptor = security / "device_identity.json"
    if not descriptor.exists():
        return
    device_id = json.loads(descriptor.read_text(encoding="utf-8"))["deviceId"]
    uuid.UUID(device_id)
    helper = Path(os.environ.get(
        "WRC_IDENTITY_TEST_EXECUTABLE",
        str(executable.resolve().parents[1] / "cmake-release" / "tests" / "Release"
            / "wrc_device_identity_tests.exe"),
    ))
    if not helper.is_file():
        raise RuntimeError(f"Identity cleanup helper is missing: {helper}; profile={profile}")
    environment = os.environ.copy()
    environment["PATH"] = str(executable.resolve().parent) + os.pathsep + environment.get("PATH", "")
    subprocess.run(
        [str(helper), "--profile-child", str(security.resolve()), "cleanup", device_id],
        env=environment, check=True, timeout=20, capture_output=True,
    )
