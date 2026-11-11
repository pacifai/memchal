"""Shared statistics for run_trials.py and plot.py."""
from __future__ import annotations
import json, math
from dataclasses import dataclass

# Must match kMinTail in the verifier.
MIN_TAIL = 10

# The formats this code reads: one verifier run, and run_trials.py's summary.
VERIFIER_SCHEMA = "memchal.verifier.v1"
TRIALS_SCHEMA = "memchal.trials.v1"


def tail_count(p: float, n: int) -> int:
    return max(0, round((1.0 - p / 100.0) * n))


def reliable(p: float, n: int) -> bool:
    """True if percentile p has at least MIN_TAIL tail samples at size n; min and max always do."""
    if p <= 0.0 or p >= 100.0:
        return True
    return tail_count(p, n) >= MIN_TAIL


def load_json(path: str, schema: str) -> dict:
    """Parses a JSON file and exits unless it declares the given schema."""
    with open(path) as f:
        doc = json.load(f)
    if doc.get("schema") != schema:
        raise SystemExit(f"{path}: schema is {doc.get('schema')!r}, expected {schema!r}")
    return doc


@dataclass
class Agg:
    """Mean, sample sd, and standard error of the mean of a scalar across runs."""
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
    """A difference with its propagated standard error; separable when it exceeds the sigma threshold.

    insufficient is set when the error is zero, which happens with a single sample or no spread.
    """
    value: float
    error: float
    sigmas_observed: float
    separable: bool
    insufficient: bool = False

    def __str__(self) -> str:
        if self.insufficient:
            return f"{self.value:+.1f}  NOT SEPARABLE: no spread to measure at this n"
        tag = "" if self.separable else "  NOT SEPARABLE at this n"
        return f"{self.value:+.1f} ± {self.error:.1f}  ({self.sigmas_observed:.1f}σ){tag}"


def _diff(value: float, error: float, sigmas: float) -> Diff:
    if error <= 0:
        return Diff(value, 0.0, 0.0, False, True)
    obs = abs(value) / error
    return Diff(value, error, obs, obs >= sigmas)


def diff(a: Agg, b: Agg, sigmas: float = 2.0) -> Diff:
    """a - b with error propagated from the standard errors of the means."""
    return _diff(a.mean - b.mean, math.hypot(a.sem, b.sem), sigmas)


def paired_diff(a_list: list, b_list: list, sigmas: float = 2.0) -> Diff:
    """Aggregates a_list[i] - b_list[i]; both lists must be equal length and ordered by cycle."""
    if len(a_list) != len(b_list):
        raise ValueError(f"paired_diff: lists must be equal length ({len(a_list)} vs {len(b_list)})")
    agg = aggregate([float(a) - float(b) for a, b in zip(a_list, b_list)])
    return _diff(agg.mean, agg.sem, sigmas)


@dataclass
class Ratio:
    """Ratio of two aggregate means with propagated relative error."""
    value: float
    rel_error: float
    error: float
    insufficient: bool = False

    def __str__(self) -> str:
        if self.insufficient:
            return f"{self.value:.2f}x (1 cycle)"
        return f"{self.value:.2f}x ± {self.error:.2f}"


def ratio(a: Agg, b: Agg) -> Ratio:
    """a.mean / b.mean with propagated error from standard errors of the means."""
    if b.mean == 0:
        return Ratio(float("inf"), float("inf"), float("inf"))
    v = a.mean / b.mean
    rel_a = (a.sem / a.mean) if a.mean != 0 else 0.0
    rel_b = (b.sem / b.mean) if b.mean != 0 else 0.0
    rel_err = math.sqrt(rel_a ** 2 + rel_b ** 2)
    err = v * rel_err
    return Ratio(v, rel_err, err, a.n < 2 or b.n < 2)


def trend(values: list) -> dict:
    """Compares the first and last thirds of ordered values with diff().

    Returns a dict of first, last, diff, drifted, and insufficient_data (a third has under 2 values).
    """
    vals = [float(v) for v in values]
    n = len(vals)
    third = max(1, n // 3)
    first_agg = aggregate(vals[:third])
    last_agg = aggregate(vals[n - third:])
    d = diff(last_agg, first_agg)
    insufficient = first_agg.n < 2 or last_agg.n < 2 or d.insufficient
    drifted = d.separable and not insufficient
    return {"first": first_agg.__dict__, "last": last_agg.__dict__,
            "diff": d.__dict__, "drifted": drifted,
            "insufficient_data": insufficient}


def step_change(values: list, rel_threshold: float = 0.25) -> dict:
    """Find the largest relative jump between consecutive values.

    Returns {"max_rel_jump": float, "at_index": int, "flag": bool}.
    """
    vals = [float(v) for v in values]
    max_rel = 0.0
    at_idx = 0
    for i in range(1, len(vals)):
        mid = (vals[i] + vals[i - 1]) / 2.0
        if mid == 0:
            continue
        rel = abs(vals[i] - vals[i - 1]) / mid
        if rel > max_rel:
            max_rel = rel
            at_idx = i
    return {"max_rel_jump": max_rel, "at_index": at_idx, "flag": max_rel >= rel_threshold}
