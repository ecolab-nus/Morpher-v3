#!/usr/bin/env python3
"""Run a fixed benchmark suite and fail on achieved-II regressions."""

from __future__ import annotations

import argparse
import csv
import os
import re
import signal
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml


def terminate_process_group(process: subprocess.Popen, grace_sec: float = 3.0) -> None:
    try:
        pgid = os.getpgid(process.pid)
    except ProcessLookupError:
        return

    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return

    try:
        process.wait(timeout=grace_sec)
        return
    except subprocess.TimeoutExpired:
        pass

    try:
        os.killpg(pgid, signal.SIGKILL)
    except ProcessLookupError:
        return
    process.wait()


def run_process_group(cmd: list[str], *, cwd: Path) -> subprocess.CompletedProcess:
    process = subprocess.Popen(cmd, cwd=cwd, start_new_session=True)
    try:
        process.wait()
    except BaseException:
        terminate_process_group(process)
        raise
    return subprocess.CompletedProcess(cmd, process.returncode)


@dataclass(frozen=True)
class BenchmarkExpectation:
    benchmark: str
    function: str
    max_ii: int
    max_mismatches: int | None = None


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    repo_root = default_repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=repo_root / "scripts" / "regression_benchmarks.yaml",
    )
    parser.add_argument("--suite", default="ci_4x4")
    parser.add_argument("--config", help="Override manifest config.")
    parser.add_argument("--runmode", help="Override manifest runmode.")
    parser.add_argument("--jobs", type=int, help="Override manifest jobs.")
    parser.add_argument("--timeout-sec", type=int, help="Override manifest timeout_sec.")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory relative to --repo-root unless absolute.",
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def load_suite(manifest_path: Path, suite_name: str) -> dict[str, Any]:
    with manifest_path.open("r", encoding="utf-8") as stream:
        manifest = yaml.safe_load(stream)
    suites = manifest.get("suites") if isinstance(manifest, dict) else None
    if not isinstance(suites, dict) or suite_name not in suites:
        available = ", ".join(sorted(suites)) if isinstance(suites, dict) else "<none>"
        raise ValueError(f"Suite '{suite_name}' not found. Available: {available}")
    suite = suites[suite_name]
    if not isinstance(suite, dict):
        raise ValueError(f"Suite '{suite_name}' must be a mapping.")
    return suite


def parse_expectations(suite: dict[str, Any]) -> list[BenchmarkExpectation]:
    raw_benchmarks = suite.get("benchmarks")
    if not isinstance(raw_benchmarks, list) or not raw_benchmarks:
        raise ValueError("Regression suite must define a non-empty benchmarks list.")

    expectations: list[BenchmarkExpectation] = []
    seen: set[str] = set()
    for item in raw_benchmarks:
        if not isinstance(item, dict):
            raise ValueError("Each benchmark expectation must be a mapping.")
        benchmark = item.get("benchmark")
        function = item.get("function")
        max_ii = item.get("max_ii")
        max_mismatches = item.get("max_mismatches")
        if not isinstance(benchmark, str) or not benchmark:
            raise ValueError("Each benchmark must define non-empty string 'benchmark'.")
        if not isinstance(function, str) or not function:
            raise ValueError(f"{benchmark}: missing non-empty string 'function'.")
        if not isinstance(max_ii, int) or max_ii <= 0:
            raise ValueError(f"{benchmark}: max_ii must be a positive integer.")
        if max_mismatches is not None and (
            not isinstance(max_mismatches, int) or max_mismatches < 0
        ):
            raise ValueError(f"{benchmark}: max_mismatches must be a non-negative integer.")
        if benchmark in seen:
            raise ValueError(f"Duplicate benchmark in suite: {benchmark}")
        seen.add(benchmark)
        expectations.append(
            BenchmarkExpectation(
                benchmark=benchmark,
                function=function,
                max_ii=max_ii,
                max_mismatches=max_mismatches,
            )
        )
    return expectations


def suite_value(args: argparse.Namespace, suite: dict[str, Any], key: str, default: Any) -> Any:
    arg_name = key.replace("_", "-")
    override = getattr(args, arg_name.replace("-", "_"), None)
    if override is not None:
        return override
    return suite.get(key, default)


def run_all_command(
    *,
    repo_root: Path,
    expectations: list[BenchmarkExpectation],
    config: str,
    runmode: str,
    jobs: int,
    timeout_sec: int,
    output_dir: Path,
    dry_run: bool,
) -> tuple[list[str], Path]:
    output_dir = output_dir if output_dir.is_absolute() else repo_root / output_dir
    output_csv = output_dir / "results.csv"

    def display_path(path: Path) -> str:
        try:
            return str(path.relative_to(repo_root))
        except ValueError:
            return str(path)

    cmd = [
        sys.executable,
        str(repo_root / "scripts" / "run_all_benchmarks.py"),
        "--repo-root",
        str(repo_root),
        "--config",
        config,
        "--runmode",
        runmode,
        "--jobs",
        str(jobs),
        "--timeout-sec",
        str(timeout_sec),
        "--output-csv",
        display_path(output_csv),
    ]
    for expectation in expectations:
        cmd.extend(["--filter", f"^{re.escape(expectation.benchmark)}$"])
    if dry_run:
        cmd.append("--dry-run")
    return cmd, output_csv


def read_results(csv_path: Path) -> dict[str, dict[str, str]]:
    if not csv_path.exists():
        raise FileNotFoundError(f"Regression CSV not found: {csv_path}")
    with csv_path.open("r", encoding="utf-8", newline="") as stream:
        return {row["benchmark"]: row for row in csv.DictReader(stream)}


def validate_results(
    expectations: list[BenchmarkExpectation], rows: dict[str, dict[str, str]]
) -> list[str]:
    failures: list[str] = []
    for expected in expectations:
        row = rows.get(expected.benchmark)
        if row is None:
            failures.append(f"{expected.benchmark}: missing from results")
            continue

        status = row.get("status", "")
        if status != "ok":
            failures.append(
                f"{expected.benchmark}: status={status or '<empty>'}, "
                f"return_code={row.get('return_code', '')}"
            )
            continue

        raw_ii = row.get("achieved_ii", "")
        try:
            achieved_ii = int(raw_ii)
        except ValueError:
            failures.append(f"{expected.benchmark}: missing/invalid achieved_ii={raw_ii!r}")
            continue
        if achieved_ii > expected.max_ii:
            failures.append(
                f"{expected.benchmark}: achieved II {achieved_ii} > max II {expected.max_ii}"
            )

        if expected.max_mismatches is not None:
            raw_mismatches = row.get("mismatches", "")
            mismatches = 0 if raw_mismatches == "" else int(raw_mismatches)
            if mismatches > expected.max_mismatches:
                failures.append(
                    f"{expected.benchmark}: mismatches {mismatches} > "
                    f"max {expected.max_mismatches}"
                )
    return failures


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest if args.manifest.is_absolute() else repo_root / args.manifest
    suite = load_suite(manifest_path, args.suite)
    expectations = parse_expectations(suite)

    config = suite_value(args, suite, "config", "config/default.yaml")
    runmode = suite_value(args, suite, "runmode", "runall")
    jobs = int(suite_value(args, suite, "jobs", 1))
    timeout_sec = int(suite_value(args, suite, "timeout_sec", 0))

    print(f"Regression suite: {args.suite}")
    if suite.get("description"):
        print(f"Description: {suite['description']}")
    print(f"Benchmarks: {len(expectations)}")
    for item in expectations:
        print(f"  {item.benchmark} :: max_ii={item.max_ii}")
    sys.stdout.flush()

    output_dir = args.output_dir or Path("generated") / "morpher_runs" / "regression" / args.suite
    resolved_output_dir = output_dir if output_dir.is_absolute() else repo_root / output_dir
    if not args.dry_run:
        try:
            (resolved_output_dir / "results.csv").unlink()
        except FileNotFoundError:
            pass

    cmd, output_csv = run_all_command(
        repo_root=repo_root,
        expectations=expectations,
        config=str(config),
        runmode=str(runmode),
        jobs=jobs,
        timeout_sec=timeout_sec,
        output_dir=output_dir,
        dry_run=args.dry_run,
    )
    print("\n$ " + " ".join(cmd))
    sys.stdout.flush()
    completed = run_process_group(cmd, cwd=repo_root)
    if args.dry_run:
        return completed.returncode

    rows = read_results(output_csv)
    failures = validate_results(expectations, rows)
    if failures:
        print("\nRegression failures:")
        for failure in failures:
            print(f"  - {failure}")
        print(f"\nCSV: {output_csv}")
        return 1

    print("\nRegression suite passed.")
    print(f"CSV: {output_csv}")
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
