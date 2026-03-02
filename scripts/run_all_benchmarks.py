#!/usr/bin/env python3
"""

Benchmarks are auto-discovered from:
  Morpher_DFG_Generator/benchmarks/<suite>/<kernel>/<kernel>.c

To add new benchmarks, add a new suite/kernel subtree in that format.
The script validates that every benchmark .c file follows this layout.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import yaml


@dataclass(frozen=True)
class BenchmarkSpec:
    rel_source: str
    function: str


BMARK_ROOT = Path("Morpher_DFG_Generator") / "benchmarks"


@dataclass
class RunResult:
    idx: int
    total: int
    rel_source: str
    function: str
    return_code: int | None
    timeout: bool
    elapsed_sec: float
    log_file: Path
    achieved_ii: int | None
    ii_source: str | None
    mapping_file: Path | None
    result_file: Path | None
    matches: int | None
    mismatches: int | None

    @property
    def status(self) -> str:
        if self.timeout:
            return "timeout"
        if self.return_code == 0:
            return "ok"
        return "failed"


def read_mapper_subfolder(repo_root: Path, config_rel: str) -> str:
    config_path = repo_root / config_rel
    if not config_path.exists():
        raise FileNotFoundError(f"Config file not found: {config_path}")
    with config_path.open("r", encoding="utf-8") as stream:
        config_dict = yaml.safe_load(stream)
    if not isinstance(config_dict, dict):
        raise ValueError(f"Config {config_path} must contain a top-level mapping.")
    mapper_subfolder = config_dict.get("mapper_subfolder")
    if not isinstance(mapper_subfolder, str) or not mapper_subfolder.strip():
        raise ValueError(
            f"Config {config_path} must define non-empty string key 'mapper_subfolder'."
        )
    return mapper_subfolder.strip()


_SIM_RESULT_RE = re.compile(
    r"Simulation Result:\s*Matches::\s*(\d+)\s*,\s*Mismatches::\s*(\d+)"
)
_MAP_SUCCESS_II_RE = re.compile(r"Map Success with II =\s*(\d+)")
_II_IN_FILENAME_RE = re.compile(r"_II=(\d+)")


def parse_matches_mismatches(log_text: str) -> tuple[int | None, int | None]:
    last_match = None
    for match in _SIM_RESULT_RE.finditer(log_text):
        last_match = match
    if not last_match:
        return None, None
    return int(last_match.group(1)), int(last_match.group(2))


def parse_ii_from_log(log_text: str) -> int | None:
    last_match = None
    for match in _MAP_SUCCESS_II_RE.finditer(log_text):
        last_match = match
    if not last_match:
        return None
    return int(last_match.group(1))


def newest_ii_from_bins(bin_files: list[Path], start_time: float) -> int | None:
    newest_mtime = -1.0
    newest_ii = None
    for bin_file in bin_files:
        name_match = _II_IN_FILENAME_RE.search(bin_file.name)
        if not name_match:
            continue
        try:
            mtime = bin_file.stat().st_mtime
        except OSError:
            continue
        if mtime < start_time:
            continue
        if mtime > newest_mtime:
            newest_mtime = mtime
            newest_ii = int(name_match.group(1))
    return newest_ii


def newest_ii_from_files(files: list[Path], start_time: float) -> tuple[int | None, Path | None]:
    newest_mtime = -1.0
    newest_ii = None
    newest_file = None
    for file_path in files:
        name_match = _II_IN_FILENAME_RE.search(file_path.name)
        if not name_match:
            continue
        try:
            mtime = file_path.stat().st_mtime
        except OSError:
            continue
        if mtime < start_time:
            continue
        if mtime > newest_mtime:
            newest_mtime = mtime
            newest_ii = int(name_match.group(1))
            newest_file = file_path
    return newest_ii, newest_file


def newest_file_for_ii(files: list[Path], start_time: float, ii: int) -> Path | None:
    newest_mtime = -1.0
    newest_file = None
    for file_path in files:
        name_match = _II_IN_FILENAME_RE.search(file_path.name)
        if not name_match:
            continue
        if int(name_match.group(1)) != ii:
            continue
        try:
            mtime = file_path.stat().st_mtime
        except OSError:
            continue
        if mtime < start_time:
            continue
        if mtime > newest_mtime:
            newest_mtime = mtime
            newest_file = file_path
    return newest_file


_RESULT_LINE_II_RE = re.compile(r"method:\S+\s+(\d+)\s+status:(?:success|failed)\s+\d+\s*$")


def ii_from_result_txt(result_file: Path, start_time: float) -> int | None:
    if not result_file.exists():
        return None
    try:
        if result_file.stat().st_mtime < start_time:
            return None
        text = result_file.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None

    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        return None
    match = _RESULT_LINE_II_RE.search(lines[-1])
    if not match:
        return None
    return int(match.group(1))


def parse_args() -> argparse.Namespace:
    default_jobs = min(8, max(1, os.cpu_count() or 1))
    default_repo = Path(__file__).resolve().parents[1]

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=default_repo,
        help="Morpher repository root. Default: parent of this script.",
    )
    parser.add_argument(
        "--config",
        default="config/default_config.yaml",
        help="Config path relative to --repo-root.",
    )
    parser.add_argument(
        "--runmode",
        default="runall",
        choices=["runall", "dfg_gen_only", "mapper_only", "sim_only"],
        help="Run mode passed to run_morpher.py.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=default_jobs,
        help=f"Number of parallel benchmark jobs (default: {default_jobs}).",
    )
    parser.add_argument(
        "--timeout-sec",
        type=int,
        default=0,
        help="Per benchmark timeout in seconds (0 = no timeout).",
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=Path("benchmark_run_logs"),
        help="Per-benchmark log directory, relative to --repo-root.",
    )
    parser.add_argument(
        "--filter",
        action="append",
        default=[],
        help="Regex filter on benchmark source path (repeatable).",
    )
    parser.add_argument(
        "--max-benches",
        type=int,
        default=0,
        help="Limit benchmark count after filtering (0 = all).",
    )
    parser.add_argument(
        "--output-csv",
        type=Path,
        default=Path("benchmark_run_results.csv"),
        help="Output CSV path, relative to --repo-root.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate and print benchmark list without executing.",
    )
    return parser.parse_args()


def sanitize_name(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "__", text)


def discover_all_benchmarks(repo_root: Path) -> list[BenchmarkSpec]:
    bench_root = repo_root / BMARK_ROOT
    if not bench_root.exists():
        raise FileNotFoundError(f"Benchmark root not found: {bench_root}")

    all_c_files = sorted(bench_root.rglob("*.c"))
    invalid_layout: list[str] = []
    normalized: list[BenchmarkSpec] = []
    seen_rel: set[str] = set()
    duplicates: list[str] = []

    for c_file in all_c_files:
        rel = c_file.relative_to(bench_root).as_posix()
        rel_path = Path(rel)

        valid_layout = (
            len(rel_path.parts) == 3
            and rel_path.name == f"{rel_path.parent.name}.c"
        )
        if not valid_layout:
            invalid_layout.append(rel)
            continue

        function = rel_path.stem
        if not re.fullmatch(r"[A-Za-z_]\w*", function):
            invalid_layout.append(rel)
            continue

        if rel in seen_rel:
            duplicates.append(rel)
            continue
        seen_rel.add(rel)
        normalized.append(BenchmarkSpec(rel_source=rel, function=function))

    if invalid_layout:
        bad = "\n  - ".join(invalid_layout[:40])
        extra = "" if len(invalid_layout) <= 40 else f"\n  ... and {len(invalid_layout) - 40} more"
        raise ValueError(
            "Found benchmark .c files outside required layout "
            "'<suite>/<kernel>/<kernel>.c':\n"
            f"  - {bad}{extra}"
        )

    if duplicates:
        dupes = "\n  - ".join(sorted(duplicates))
        raise ValueError(f"Duplicate benchmark entries discovered:\n  - {dupes}")

    if not normalized:
        raise ValueError(f"No benchmarks discovered under {bench_root}")

    return normalized


def select_benchmarks(
    discovered: list[BenchmarkSpec], args: argparse.Namespace
) -> list[BenchmarkSpec]:
    selected = list(discovered)
    if args.filter:
        compiled = [re.compile(pattern) for pattern in args.filter]
        selected = [
            spec
            for spec in selected
            if any(regex.search(spec.rel_source) for regex in compiled)
        ]
    if args.max_benches and args.max_benches > 0:
        selected = selected[: args.max_benches]
    return selected


def run_one(
    idx: int,
    total: int,
    spec: BenchmarkSpec,
    *,
    repo_root: Path,
    config_rel: str,
    runmode: str,
    timeout_sec: int,
    log_dir: Path,
    mapper_subfolder: str,
) -> RunResult:
    safe = sanitize_name(spec.rel_source)
    log_file = log_dir / f"{idx:03d}_{safe}.log"

    cmd = [
        sys.executable,
        "-u",
        "run_morpher.py",
        spec.rel_source,
        spec.function,
        config_rel,
        runmode,
    ]
    start = time.time()
    rc = None
    timed_out = False

    appfolder = str(Path(spec.rel_source).parent).replace("\\", "/")
    mapper_kernel_dir = (
        repo_root / "Morpher_CGRA_Mapper" / "benchmarks" / mapper_subfolder / appfolder
    )
    sim_kernel_dir = repo_root / "hycube_simulator" / "benchmarks" / appfolder

    try:
        with log_file.open("w", encoding="utf-8") as log:
            completed = subprocess.run(
                cmd,
                cwd=repo_root,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=(None if timeout_sec <= 0 else timeout_sec),
                check=False,
            )
        rc = completed.returncode
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        with log_file.open("a", encoding="utf-8") as log:
            log.write(f"\n[TIMEOUT] command exceeded {timeout_sec}s\n")
            if exc.stdout:
                log.write(str(exc.stdout))
            if exc.stderr:
                log.write(str(exc.stderr))

    log_text = log_file.read_text(encoding="utf-8", errors="ignore")
    matches, mismatches = parse_matches_mismatches(log_text)
    mapper_bins = list(mapper_kernel_dir.glob("*.bin")) if mapper_kernel_dir.exists() else []
    sim_bins = list(sim_kernel_dir.glob("*.bin")) if sim_kernel_dir.exists() else []
    mapping_files = (
        list(mapper_kernel_dir.glob("*.mappingwithlatency.txt"))
        if mapper_kernel_dir.exists()
        else []
    )
    result_file = mapper_kernel_dir / "result.txt"

    achieved_ii = None
    ii_source = None
    mapping_file = None

    res_ii = ii_from_result_txt(result_file, start)
    if res_ii is not None:
        achieved_ii = res_ii
        ii_source = "result_txt"
        mapping_file = newest_file_for_ii(mapping_files, start, achieved_ii)
    else:
        map_ii, map_file = newest_ii_from_files(mapping_files, start)
        if map_ii is not None:
            achieved_ii = map_ii
            ii_source = "mapping_txt"
            mapping_file = map_file
        else:
            bin_ii = newest_ii_from_bins(mapper_bins + sim_bins, start)
            if bin_ii is not None:
                achieved_ii = bin_ii
                ii_source = "bin_file"
            else:
                log_ii = parse_ii_from_log(log_text)
                if log_ii is not None:
                    achieved_ii = log_ii
                    ii_source = "mapper_log"

    elapsed = round(time.time() - start, 2)
    return RunResult(
        idx=idx,
        total=total,
        rel_source=spec.rel_source,
        function=spec.function,
        return_code=rc,
        timeout=timed_out,
        elapsed_sec=elapsed,
        log_file=log_file,
        achieved_ii=achieved_ii,
        ii_source=ii_source,
        mapping_file=mapping_file,
        result_file=(result_file if result_file.exists() else None),
        matches=matches,
        mismatches=mismatches,
    )


def write_csv(results: list[RunResult], out_csv: Path) -> None:
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "benchmark",
        "function",
        "status",
        "return_code",
        "achieved_ii",
        "ii_source",
        "matches",
        "mismatches",
        "timeout",
        "elapsed_sec",
        "log_file",
        "mapping_file",
        "result_file",
    ]
    with out_csv.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=fields)
        writer.writeheader()
        for r in results:
            writer.writerow(
                {
                    "benchmark": r.rel_source,
                    "function": r.function,
                    "status": r.status,
                    "return_code": r.return_code,
                    "achieved_ii": r.achieved_ii,
                    "ii_source": r.ii_source,
                    "matches": r.matches,
                    "mismatches": r.mismatches,
                    "timeout": r.timeout,
                    "elapsed_sec": r.elapsed_sec,
                    "log_file": str(r.log_file),
                    "mapping_file": (str(r.mapping_file) if r.mapping_file else ""),
                    "result_file": (str(r.result_file) if r.result_file else ""),
                }
            )


def main() -> int:
    args = parse_args()
    if args.jobs <= 0:
        raise ValueError("--jobs must be >= 1")

    repo_root = args.repo_root.resolve()
    if not (repo_root / "run_morpher.py").exists():
        raise FileNotFoundError(f"run_morpher.py not found under {repo_root}")
    mapper_subfolder = read_mapper_subfolder(repo_root, args.config)

    discovered = discover_all_benchmarks(repo_root)
    selected = select_benchmarks(discovered, args)
    if not selected:
        print("No benchmarks matched filters.")
        return 1

    if args.dry_run:
        print(
            f"Dry-run benchmark list ({len(selected)} selected / "
            f"{len(discovered)} discovered):"
        )
        for idx, spec in enumerate(selected, start=1):
            print(f"{idx:03d} {spec.rel_source} :: {spec.function}")
        return 0

    log_dir = (repo_root / args.log_dir).resolve()
    output_csv = (repo_root / args.output_csv).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)
    total = len(selected)
    print(f"Launching {total} benchmarks with {args.jobs} parallel worker(s)...")

    indexed_results: list[tuple[int, RunResult]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(
                run_one,
                idx,
                total,
                spec,
                repo_root=repo_root,
                config_rel=args.config,
                runmode=args.runmode,
                timeout_sec=args.timeout_sec,
                log_dir=log_dir,
                mapper_subfolder=mapper_subfolder,
            ): idx
            for idx, spec in enumerate(selected, start=1)
        }
        for future in concurrent.futures.as_completed(futures):
            idx = futures[future]
            result = future.result()
            mapping_name = result.mapping_file.name if result.mapping_file else "-"
            print(
                f"[{result.idx}/{result.total}] {result.rel_source} :: "
                f"{result.status} rc={result.return_code} "
                f"ii={result.achieved_ii} ii_src={result.ii_source} "
                f"mismatches={result.mismatches} map={mapping_name} "
                f"elapsed={result.elapsed_sec}s"
            )
            indexed_results.append((idx, result))

    indexed_results.sort(key=lambda item: item[0])
    results = [result for _, result in indexed_results]
    failed = sum(1 for r in results if r.status != "ok")
    write_csv(results, output_csv)

    print("\nSummary:")
    print(f"  total benchmarks: {len(results)}")
    print(f"  failed benchmarks: {failed}")
    print(f"  csv: {output_csv}")
    print(f"  logs: {log_dir}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
