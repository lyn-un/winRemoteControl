"""Runner contract tests with fake child processes; never start a GPU workload."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "gpu_matrix_runner", ROOT / "tools/performance/run-gpu-encoding-benchmark.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class MatrixRunnerTest(unittest.TestCase):
    def run_case(self, *, invalid=False, skip=False, change_binary=False):
        with tempfile.TemporaryDirectory(prefix="matrix-runner-test-", dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            executable = directory / "fake.exe"
            executable.write_bytes(b"fixture-only-not-executable")
            output = directory / "results"
            calls = []

            class Child:
                pid = 123

                def __init__(self, command, stdout, **kwargs):
                    calls.append(command)
                    value = {"valid": not invalid, "smokeOnly": False,
                             "mode": command[1], "encoder": command[2],
                             "width": int(command[3]), "height": int(command[4]),
                             "fps": int(command[5]), "repetition": int(command[6]),
                             "warmupSeconds": 10, "sampleSeconds": 60,
                             "submitted": 2100, "decoded": 2100}
                    stdout.write(json.dumps(value) + "\n")

                def wait(self, timeout):
                    if change_binary:
                        executable.write_bytes(b"changed-fixture")
                    return 77 if skip else 0

            arguments = ["runner", "--backend", "h264_amf", "--executable", str(executable),
                         "--output-dir", str(output)]
            with patch("sys.argv", arguments), patch.object(RUNNER.subprocess, "Popen", Child), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = RUNNER.main()
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            return code, calls, manifest

    def test_complete_matrix_records_same_build(self):
        code, calls, manifest = self.run_case()
        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 24)
        self.assertEqual(len(manifest["cases"]), 24)
        self.assertEqual(len(manifest["executableSha256"]), 64)
        # Repetition two reverses the immediate CPU/GPU predecessor.
        self.assertEqual([command[1] for command in calls[:6]],
                         ["cpu", "gpu", "gpu", "cpu", "cpu", "gpu"])

    def test_invalid_child_result_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            self.run_case(invalid=True)

    def test_binary_change_rejected_before_next_case(self):
        with self.assertRaisesRegex(RuntimeError, "binary changed"):
            self.run_case(change_binary=True)

    def test_unavailable_adapter_is_explicit_skip(self):
        code, calls, manifest = self.run_case(skip=True)
        self.assertEqual(code, 77)
        self.assertEqual(len(calls), 1)
        self.assertEqual(manifest["cases"], [])


if __name__ == "__main__":
    unittest.main()
