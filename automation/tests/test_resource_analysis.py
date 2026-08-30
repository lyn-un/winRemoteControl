import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from wrcdriver.resources import (
    ResourceSample,
    metric_trend,
    parse_resource_trace,
    resource_summary_is_stable,
    settled_session_samples,
    summarize_resource_samples,
)


def _sample(generation: int, handles: int, private_bytes: int) -> ResourceSample:
    return ResourceSample(
        pid=100,
        role="controlled",
        generation=generation,
        stage="session_listening",
        stale=False,
        privateBytes=private_bytes,
        workingSetBytes=private_bytes,
        handles=handles,
        threads=5,
        gpuDedicatedBytes=0,
        gpuSharedBytes=0,
    )


class ResourceAnalysisTests(unittest.TestCase):
    def test_parser_ignores_malformed_and_stale_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "resource.log"
            path.write_text(
                "ignored\n"
                "2026-08-30 12:00:00.000 [RESOURCE_TRACE] pid=100 role=controlled "
                "generation=2 stage=session_listening stale=0 privateBytes=10 "
                "workingSetBytes=20 handles=30 threads=4 gpuDedicatedBytes=5 "
                "gpuSharedBytes=6\n"
                "2026-08-30 12:00:01.000 [RESOURCE_TRACE] pid=100 role=controlled "
                "generation=1 stage=session_listening stale=1 privateBytes=99 "
                "workingSetBytes=99 handles=99 threads=99 gpuDedicatedBytes=99 "
                "gpuSharedBytes=99\n",
                encoding="utf-8",
            )
            samples = parse_resource_trace(path)
            self.assertEqual(2, len(samples))
            settled = settled_session_samples(samples, "controlled")
            self.assertEqual([2], [sample.generation for sample in settled])
            self.assertEqual(30, settled[0].handles)

    def test_slope_uses_cycle_index(self):
        samples = [_sample(index, 100 + index * 3, 1_000) for index in range(1, 6)]
        self.assertAlmostEqual(3.0, metric_trend(samples, "handles").slopePerCycle)

    def test_stability_rejects_linear_growth_after_warmup(self):
        samples = [
            _sample(index, 200 + index * 2, 10_000_000 + index * 4_000_000)
            for index in range(1, 7)
        ]
        summary = summarize_resource_samples(samples, warmup_cycles=1)
        stable, failures = resource_summary_is_stable(summary)
        self.assertFalse(stable)
        self.assertTrue(any("handles" in failure for failure in failures))
        self.assertTrue(any("privateBytes" in failure for failure in failures))

    def test_stability_accepts_plateau(self):
        samples = [
            _sample(index, 200, 10_000_000 + (index % 2) * 100_000)
            for index in range(1, 7)
        ]
        stable, failures = resource_summary_is_stable(
            summarize_resource_samples(samples, warmup_cycles=1)
        )
        self.assertTrue(stable)
        self.assertEqual([], failures)


if __name__ == "__main__":
    unittest.main()
