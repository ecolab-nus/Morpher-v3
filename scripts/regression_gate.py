#!/usr/bin/env python3
"""One-command regression gate for the Morpher pipeline.

Builds the mapper, then runs a small set of fast kernels end-to-end
(DFG gen -> map -> simulate) with config/regression.yaml and asserts that
mapping still succeeds at the expected II and that simulation validation stays
clean. This is the guard to run after any mapper/DFG/simulator change.

Hard failures (exit non-zero):
  - run did not complete / no "Map Success"
  - achieved II is higher than expected (II regression)
  - simulator mismatches != 0, or matches != expected

Soft (warn unless --strict): achieved II lower than expected (tighten the
expectation), or mapper time exceeding max(5s, 3x baseline).

Usage:
  python3 scripts/regression_gate.py                 # build + run all
  python3 scripts/regression_gate.py --filter fir    # subset by kernel name
  python3 scripts/regression_gate.py --no-build       # skip cmake build
  python3 scripts/regression_gate.py --strict         # promote soft warns to failures
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
MAPPER_HOME = REPO_ROOT / "Morpher_CGRA_Mapper"
DEFAULT_EXPECTATIONS = REPO_ROOT / "scripts" / "regression_expectations.yaml"

_MAP_SUCCESS_RE = re.compile(r"Map Success with II =\s*(\d+)")
_FINAL_MATCH_RE = re.compile(r"Matches:\s*(\d+)\s+Mismatches:\s*(\d+)")
_MAPPER_SEC_RE = re.compile(r"Elapsed time in seconds:\s*(\d+)\s*sec")


class Outcome:
    def __init__(self, name):
        self.name = name
        self.ii = None
        self.matches = None
        self.mismatches = None
        self.mapper_sec = None
        self.wall_sec = None
        self.completed = False
        self.hard_fail = []   # list[str]
        self.soft_warn = []   # list[str]


def build_mapper() -> bool:
    print("[regression] building mapper ...", flush=True)
    rc = subprocess.run(
        ["cmake", "--build", "build"],
        cwd=str(MAPPER_HOME),
    ).returncode
    if rc != 0:
        print("[regression] BUILD FAILED", flush=True)
        return False
    print("[regression] build ok", flush=True)
    return True


def run_kernel(source: str, function: str, config: str, timeout_sec: int) -> tuple[str, float]:
    """Run one kernel end-to-end; return (combined_output, wall_seconds)."""
    cmd = [
        sys.executable, "-u", "run_morpher.py",
        source, function, config, "runall",
    ]
    start = time.monotonic()
    proc = subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout_sec,
    )
    wall = time.monotonic() - start
    return proc.stdout, wall


def evaluate(spec: dict, config: str, timeout_sec: int, strict: bool) -> Outcome:
    name = Path(spec["source"]).stem
    out = Outcome(name)
    try:
        text, wall = run_kernel(spec["source"], spec["function"], config, timeout_sec)
    except subprocess.TimeoutExpired:
        out.hard_fail.append("timed out after %ds" % timeout_sec)
        return out
    out.wall_sec = wall

    ii_hits = _MAP_SUCCESS_RE.findall(text)
    if ii_hits:
        out.ii = int(ii_hits[-1])
    match_hits = _FINAL_MATCH_RE.findall(text)
    if match_hits:
        out.matches = int(match_hits[-1][0])
        out.mismatches = int(match_hits[-1][1])
    sec_hits = _MAPPER_SEC_RE.findall(text)
    if sec_hits:
        out.mapper_sec = max(int(s) for s in sec_hits)
    out.completed = out.ii is not None

    exp_ii = int(spec["expected_ii"])
    exp_matches = int(spec["expected_matches"])
    baseline_sec = int(spec.get("baseline_sec", 0))

    if out.ii is None:
        out.hard_fail.append("no 'Map Success' (mapping did not complete)")
    else:
        if out.ii > exp_ii:
            out.hard_fail.append("II regressed: got %d, expected <= %d" % (out.ii, exp_ii))
        elif out.ii < exp_ii:
            out.soft_warn.append("II improved: got %d < expected %d (tighten expectation)" % (out.ii, exp_ii))

    if out.mismatches is None:
        out.hard_fail.append("no simulation result parsed (validation did not run)")
    else:
        if out.mismatches != 0:
            out.hard_fail.append("validation mismatches=%d (expected 0)" % out.mismatches)
        if out.matches != exp_matches:
            out.hard_fail.append("matches=%s, expected %d" % (out.matches, exp_matches))

    if out.mapper_sec is not None and baseline_sec > 0:
        budget = max(5, 3 * baseline_sec)
        if out.mapper_sec > budget:
            msg = "mapper time %ds > budget %ds (3x baseline %ds)" % (out.mapper_sec, budget, baseline_sec)
            (out.hard_fail if strict else out.soft_warn).append(msg)

    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--expectations", default=str(DEFAULT_EXPECTATIONS),
                        help="Path to expectations YAML (default: scripts/regression_expectations.yaml)")
    parser.add_argument("--filter", default=None, help="Substring; only run kernels whose name matches.")
    parser.add_argument("--no-build", action="store_true", help="Skip building the mapper.")
    parser.add_argument("--strict", action="store_true", help="Treat timing/II-improvement warnings as failures.")
    parser.add_argument("--timeout-sec", type=int, default=600, help="Per-kernel wall timeout (default 600).")
    args = parser.parse_args()

    spec_doc = yaml.safe_load(Path(args.expectations).read_text(encoding="utf-8"))
    config = spec_doc.get("config", "config/regression.yaml")
    benches = spec_doc["benchmarks"]
    if args.filter:
        benches = [b for b in benches if args.filter in Path(b["source"]).stem]
    if not benches:
        print("[regression] no benchmarks match filter %r" % args.filter)
        return 1

    if not args.no_build and not build_mapper():
        return 1

    outcomes = []
    for spec in benches:
        name = Path(spec["source"]).stem
        print("\n[regression] === %s ===" % name, flush=True)
        out = evaluate(spec, config, args.timeout_sec, args.strict)
        for msg in out.hard_fail:
            print("  FAIL: %s" % msg, flush=True)
        for msg in out.soft_warn:
            print("  warn: %s" % msg, flush=True)
        if not out.hard_fail and not out.soft_warn:
            print("  ok: II=%s matches=%s mapper=%ss" % (out.ii, out.matches, out.mapper_sec), flush=True)
        outcomes.append(out)

    print("\n[regression] summary")
    print("  %-10s %-4s %-8s %-9s %-8s %s" % ("kernel", "II", "matches", "mism", "mapper_s", "verdict"))
    n_fail = 0
    for out in outcomes:
        verdict = "FAIL" if out.hard_fail else ("warn" if out.soft_warn else "ok")
        if out.hard_fail:
            n_fail += 1
        print("  %-10s %-4s %-8s %-9s %-8s %s" % (
            out.name, out.ii, out.matches, out.mismatches, out.mapper_sec, verdict))

    if n_fail:
        print("\n[regression] %d/%d kernels FAILED" % (n_fail, len(outcomes)))
        return 1
    print("\n[regression] all %d kernels passed" % len(outcomes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
