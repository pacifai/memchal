"""Shared statistics + IO for the memory-challenge harness.

Single source of truth for:
  - percentile logic (mirrors verifier.cpp: nearest-rank + the tail-support rule),
  - error propagation across repeated runs.

Used by the repeat-trials driver, the transport-floor subtraction, and both plot
scripts, so the numbers in the README cannot drift from the numbers on the charts.
"""
from __future__ import annotations
import json, math, csv
from dataclasses import dataclass

# Must match kMinTail in verifier.cpp: a percentile with fewer than this many
# samples in its tail is not trustworthy (p99.9 from n=300 has ~0 tail samples).
MIN_TAIL = 10


# --- percentiles (identical convention to verifier.cpp) ----------------------
def pctile(sorted_xs, p: float) -> float:
    if not sorted_xs:
        return 0.0
    idx = int(p / 100.0 * (len(sorted_xs) - 1))
    return sorted_xs[idx]


def tail_count(p: float, n: int) -> int:
    return max(0, round((1.0 - p / 100.0) * n))


def reliable(p: float, n: int) -> bool:
    # min/max are extremes, not inner percentiles; only inner ones need support.
    if p <= 0.0 or p >= 100.0:
        return True
    return tail_count(p, n) >= MIN_TAIL


# --- IO ----------------------------------------------------------------------
def load_json(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def load_latencies_csv(path: str) -> list[float]:
    xs = []
    with open(path) as f:
        for row in csv.DictReader(f):
            xs.append(float(row["latency_us"]))
    return xs


# --- aggregation across repeated runs ----------------------------------------
@dataclass
class Agg:
    """Central tendency AND spread of a scalar (e.g. p50) across R independent runs.

    - mean/sd describe the run-to-run distribution (sd = spread, for error bars).
    - sem = sd/sqrt(n) is the uncertainty on the *estimate of the mean*, used when
      propagating error into derived quantities (differences, floor subtraction).
    """
    n: int
    mean: float
    sd: float
    sem: float
    lo: float
    hi: float

    @property
    def cv(self) -> float:
        return 100.0 * self.sd / self.mean if self.mean else 0.0

    def __str__(self) -> str:
        return (f"{self.mean:.1f} ± {self.sd:.1f} (sd)  "
                f"[n={self.n} min={self.lo:.1f} max={self.hi:.1f} cv={self.cv:.1f}%]")


def aggregate(values) -> Agg:
    vals = [float(v) for v in values]
    n = len(vals)
    if n == 0:
        return Agg(0, 0.0, 0.0, 0.0, 0.0, 0.0)
    mean = sum(vals) / n
    var = sum((v - mean) ** 2 for v in vals) / (n - 1) if n > 1 else 0.0
    sd = math.sqrt(var)
    sem = sd / math.sqrt(n) if n > 1 else 0.0
    return Agg(n, mean, sd, sem, min(vals), max(vals))


@dataclass
class Diff:
    """Difference of two aggregates with propagated error (e.g. memory = total-floor).

    error is the propagated standard error; `separable` is True only if the
    magnitude exceeds `sigmas` propagated errors, so we never print a difference
    the data can't actually resolve.
    """
    value: float
    error: float
    sigmas_observed: float
    separable: bool

    def __str__(self) -> str:
        tag = "" if self.separable else "  NOT SEPARABLE at this n"
        return f"{self.value:+.1f} ± {self.error:.1f}  ({self.sigmas_observed:.1f}σ){tag}"


def diff(a: Agg, b: Agg, sigmas: float = 2.0) -> Diff:
    """a - b with error propagated from the standard errors of the means."""
    value = a.mean - b.mean
    error = math.hypot(a.sem, b.sem)
    obs = abs(value) / error if error > 0 else float("inf")
    return Diff(value, error, obs, obs >= sigmas)
