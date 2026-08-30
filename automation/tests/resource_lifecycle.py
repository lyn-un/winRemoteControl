from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from e2e_support import TwoProcessSession
from wrcdriver.resources import (
    parse_resource_trace,
    resource_summary_is_stable,
    settled_session_samples,
    summarize_resource_samples,
)


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run repeated same-PID remote sessions and summarize resources."
    )
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=5)
    parser.add_argument(
        "--encoder", choices=("auto", "h264_mf", "libx264"), default="auto"
    )
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--skip-stream", action="store_true")
    parser.add_argument("--settle-seconds", type=float, default=1.0)
    return parser.parse_args()


def main() -> int:
    arguments = _arguments()
    if arguments.cycles < 1 or arguments.cycles > 100:
        raise ValueError("cycles must be in the range 1..100")
    executable = arguments.executable.resolve()
    report_directory = executable.parents[1] / "diagnostics" / "reports"
    report_directory.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    scenario = "connected" if arguments.skip_stream else "streaming"
    report_path = report_directory / (
        f"resource_e2e_{arguments.encoder}_{scenario}_{stamp}.json"
    )

    session = TwoProcessSession(
        executable,
        extra_arguments=(
            "--resource-trace",
            "--diagnostic-video-encoder",
            arguments.encoder,
        ),
    )
    try:
        host_pid = session.host.pid
        controller_pid = session.controller.pid
        for cycle in range(1, arguments.cycles + 1):
            session.connect(timeout=45.0)
            if not arguments.skip_stream:
                session.start_streaming(timeout=45.0)
            session.disconnect(timeout=45.0)
            time.sleep(arguments.settle_seconds)
            print(f"cycle {cycle}/{arguments.cycles} complete", flush=True)

        log_paths = {
            "controlled": session.profile_root / "host" / "logs"
            / "resource_trace_controlled.log",
            "controller": session.profile_root / "controller" / "logs"
            / "resource_trace_controller.log",
        }
        summaries = {}
        copied_logs = {}
        stable = True
        failures = []
        for role, log_path in log_paths.items():
            destination = report_directory / f"{stamp}_{role}.log"
            shutil.copy2(log_path, destination)
            copied_logs[role] = str(destination)
            samples = settled_session_samples(parse_resource_trace(log_path), role)
            samples = samples[-arguments.cycles :]
            summary = summarize_resource_samples(samples, warmup_cycles=1)
            role_stable, role_failures = resource_summary_is_stable(summary)
            summaries[role] = summary
            stable = stable and role_stable
            failures.extend(f"{role}: {failure}" for failure in role_failures)

        report = {
            "encoder": arguments.encoder,
            "scenario": scenario,
            "cycles": arguments.cycles,
            "hostPid": host_pid,
            "controllerPid": controller_pid,
            "stable": stable,
            "failures": failures,
            "logs": copied_logs,
            "roles": summaries,
        }
        report_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(json.dumps(report, ensure_ascii=False, indent=2))
        print(f"report: {report_path}")
        return 0 if stable or not arguments.validate else 1
    finally:
        session.close()


if __name__ == "__main__":
    raise SystemExit(main())
