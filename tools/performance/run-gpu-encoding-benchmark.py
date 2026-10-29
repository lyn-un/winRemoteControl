"""Run one backend's synthetic CPU/GPU matrix serially; never capture a desktop."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", required=True, choices=("h264_qsv", "h264_amf", "h264_nvenc"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--executable", type=Path,
                        default=root / "build/cmake-release/Release/wrc_gpu_encoding_benchmark.exe")
    parser.add_argument("--smoke", action="store_true", help="Functional matrix only; never performance evidence")
    args = parser.parse_args()
    directory = args.output_dir.resolve()
    if not directory.is_relative_to((root / "build").resolve()):
        parser.error("Raw results must stay inside the ignored build directory")
    if directory.exists():
        parser.error("Use a fresh result directory; previous results will not be overwritten")
    if not args.executable.is_file():
        parser.error("Benchmark executable is missing")
    directory.mkdir(parents=True)
    executable = args.executable.resolve()
    executable_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    manifest = {"backend": args.backend, "smokeOnly": args.smoke,
                "executable": str(executable), "executableSha256": executable_hash,
                "runnerSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "cases": []}
    with (directory / "manifest.json").open("w", encoding="utf-8") as output:
        json.dump(manifest, output, ensure_ascii=False, indent=2)
    environment = os.environ.copy()
    environment["PATH"] = str(root / "build/Release") + os.pathsep + environment.get("PATH", "")
    collected = []
    for width, height in ((1920, 1080), (2560, 1440)):
        for fps in (30, 60):
            for repetition in (1, 2, 3):
                # Counterbalance the immediate predecessor without hiding execution order.
                modes = ("cpu", "gpu") if repetition != 2 else ("gpu", "cpu")
                for mode in modes:
                    name = f"{mode}-{width}x{height}-{fps}-r{repetition}"
                    if hashlib.sha256(executable.read_bytes()).hexdigest() != executable_hash:
                        raise RuntimeError("Benchmark binary changed during the matrix; refusing mixed-build results")
                    command = [str(executable), mode, args.backend,
                               str(width), str(height), str(fps), str(repetition)]
                    if args.smoke:
                        command.append("--smoke")
                    with (directory / (name + ".log")).open("w", encoding="utf-8") as output:
                        started = time.monotonic()
                        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                                   env=environment, cwd=root)
                        print(json.dumps({"stage": "started", "case": name, "pid": process.pid}), flush=True)
                        try:
                            code = process.wait(timeout=180)
                        except subprocess.TimeoutExpired:
                            process.terminate()
                            process.wait(timeout=15)
                            raise RuntimeError(f"{name}: benchmark exceeded its bounded runtime")
                    wall_seconds = time.monotonic() - started
                    raw = (directory / (name + ".log")).read_text(encoding="utf-8", errors="replace")
                    if code == 77:
                        print(json.dumps({"stage": "skipped", "backend": args.backend,
                                          "reason": "adapter_unavailable"}), flush=True)
                        return 77
                    values = []
                    for line in raw.splitlines():
                        if line.startswith("{"):
                            try:
                                values.append(json.loads(line))
                            except json.JSONDecodeError:
                                pass
                    if code != 0 or len(values) != 1:
                        raise RuntimeError(f"{name}: exit={code}; expected one JSON result\n{raw[-2000:]}")
                    value = values[0]
                    if (value.get("valid") is not True or value.get("smokeOnly") != args.smoke
                            or value.get("encoder") != args.backend or value.get("mode") != mode
                            or (value.get("width"), value.get("height"), value.get("fps")) != (width, height, fps)
                            or value.get("repetition") != repetition
                            or value.get("sampleSeconds") != (2 if args.smoke else 60)
                            or value.get("warmupSeconds") != (1 if args.smoke else 10)):
                        raise RuntimeError(f"{name}: result does not match the requested scenario")
                    collected.append(value)
                    manifest["cases"].append({"name": name, "processWallSeconds": wall_seconds,
                                              "exitCode": code})
                    with (directory / "manifest.json").open("w", encoding="utf-8") as output:
                        json.dump(manifest, output, ensure_ascii=False, indent=2)
                    with (directory / "results.json").open("w", encoding="utf-8") as output:
                        json.dump({"backend": args.backend, "smokeOnly": args.smoke,
                                   "cases": collected}, output, ensure_ascii=False, indent=2)
                    print(json.dumps({"stage": "completed", "case": name, "exitCode": code,
                                      "submitted": value["submitted"], "decoded": value["decoded"]}), flush=True)
    print(json.dumps({"stage": "matrix_completed", "cases": len(collected), "smokeOnly": args.smoke}), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
