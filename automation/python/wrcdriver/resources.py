from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
import re


_RESOURCE_LINE = re.compile(r"\[RESOURCE_TRACE\]\s+(?P<fields>.+)$")
_FIELD = re.compile(r"(?P<name>[A-Za-z][A-Za-z0-9]*)=(?P<value>\S+)")
_NUMERIC_FIELDS = (
    "privateBytes",
    "workingSetBytes",
    "handles",
    "threads",
    "gpuDedicatedBytes",
    "gpuSharedBytes",
)


@dataclass(frozen=True)
class ResourceSample:
    pid: int
    role: str
    generation: int
    stage: str
    stale: bool
    privateBytes: int
    workingSetBytes: int
    handles: int
    threads: int
    gpuDedicatedBytes: int
    gpuSharedBytes: int


@dataclass(frozen=True)
class ResourceMetricTrend:
    first: int
    last: int
    minimum: int
    maximum: int
    slopePerCycle: float


def parse_resource_trace(path: str | Path) -> list[ResourceSample]:
    samples: list[ResourceSample] = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        match = _RESOURCE_LINE.search(line)
        if not match:
            continue
        fields = {
            field.group("name"): field.group("value")
            for field in _FIELD.finditer(match.group("fields"))
        }
        required = {"pid", "role", "generation", "stage", "stale", *_NUMERIC_FIELDS}
        if not required.issubset(fields):
            continue
        try:
            samples.append(
                ResourceSample(
                    pid=int(fields["pid"]),
                    role=fields["role"],
                    generation=int(fields["generation"]),
                    stage=fields["stage"],
                    stale=fields["stale"] != "0",
                    **{name: int(fields[name]) for name in _NUMERIC_FIELDS},
                )
            )
        except ValueError:
            continue
    return samples


def settled_session_samples(
    samples: list[ResourceSample], expected_role: str
) -> list[ResourceSample]:
    expected_stage = "session_listening" if expected_role == "controlled" else "session_idle"
    latest_by_generation: dict[int, ResourceSample] = {}
    for sample in samples:
        if (
            sample.role == expected_role
            and sample.stage == expected_stage
            and not sample.stale
            and sample.generation > 0
        ):
            latest_by_generation[sample.generation] = sample
    return [latest_by_generation[key] for key in sorted(latest_by_generation)]


def metric_trend(samples: list[ResourceSample], metric: str) -> ResourceMetricTrend:
    if not samples:
        raise ValueError("At least one resource sample is required")
    values = [int(getattr(sample, metric)) for sample in samples]
    if len(values) == 1:
        slope = 0.0
    else:
        x_mean = (len(values) - 1) / 2.0
        y_mean = sum(values) / len(values)
        numerator = sum((index - x_mean) * (value - y_mean)
                        for index, value in enumerate(values))
        denominator = sum((index - x_mean) ** 2 for index in range(len(values)))
        slope = numerator / denominator
    return ResourceMetricTrend(
        first=values[0],
        last=values[-1],
        minimum=min(values),
        maximum=max(values),
        slopePerCycle=slope,
    )


def summarize_resource_samples(
    samples: list[ResourceSample], warmup_cycles: int = 1
) -> dict:
    measured = samples[warmup_cycles:]
    if not measured:
        measured = samples
    return {
        "sampleCount": len(samples),
        "measuredSampleCount": len(measured),
        "generations": [sample.generation for sample in samples],
        "metrics": {
            metric: asdict(metric_trend(measured, metric))
            for metric in _NUMERIC_FIELDS
        },
    }


def resource_summary_is_stable(summary: dict) -> tuple[bool, list[str]]:
    limits = {
        "privateBytes": 2 * 1024 * 1024,
        "gpuDedicatedBytes": 2 * 1024 * 1024,
        "gpuSharedBytes": 2 * 1024 * 1024,
        "handles": 0.25,
        "threads": 0.10,
    }
    failures: list[str] = []
    metrics = summary.get("metrics", {})
    for metric, limit in limits.items():
        slope = float(metrics.get(metric, {}).get("slopePerCycle", 0.0))
        if slope > limit:
            failures.append(f"{metric} slope {slope:.2f} exceeds {limit:.2f} per cycle")
    return not failures, failures
