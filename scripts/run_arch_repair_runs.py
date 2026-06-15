#!/usr/bin/env python3
"""Run targeted repair attempts for arch-sweep fallback and timeout rows."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from run_all_benchmarks import BenchmarkSpec
from run_all_benchmarks import CSV_FIELDS
from run_all_benchmarks import collect_run_metadata
from run_all_benchmarks import install_signal_cleanup
from run_all_benchmarks import read_mapper_subfolder
from run_all_benchmarks import run_logged_process
from run_all_benchmarks import sanitize_name
from run_all_benchmarks import workspace_run_dirs


DEFAULT_TIMEOUT_SEC = 5400
DEFAULT_SWEEP_DIR = Path("generated/morpher_runs/arch_sweeps/edge_mem_arch_sweep")
DEFAULT_OUTPUT_DIR = Path("generated/morpher_runs/repair_runs/edge_mem_arch_sweep")


@dataclass(frozen=True)
class Experiment:
    name: str
    config_path: Path
    results_csv: Path
    config: dict[str, Any]


@dataclass(frozen=True)
class RepairJob:
    phase: str
    experiment: Experiment
    row: dict[str, str]
    unroll_factor: int
    job_index: int


@dataclass(frozen=True)
class RepairResult:
    job: RepairJob
    status: str
    return_code: int | None
    timeout: bool
    elapsed_sec: float
    achieved_ii: int | None
    score: float | int | None
    selected_unroll_factor: int | None
    ii_source: str | None
    matches: int | None
    mismatches: int | None
    run_dir: Path
    log_file: Path
    run_tag: str
    config_file: Path
    mapping_file: Path | None
    result_file: Path | None
    auto_unroll_results_file: Path | None
    failure_stage: str
    failure_reason: str
    source_updated: bool = False
    update_reason: str = ""


RESULT_FIELDS = [
    "phase",
    "experiment",
    "benchmark",
    "function",
    "kernel",
    "arch",
    "spm_mode",
    "hops",
    "original_status",
    "original_recovery_reason",
    "original_unroll",
    "repair_unroll",
    "status",
    "return_code",
    "timeout",
    "elapsed_sec",
    "ii",
    "score",
    "selected_unroll",
    "failure_stage",
    "failure_reason",
    "source_updated",
    "update_reason",
    "run_tag",
    "log_file",
    "config_file",
    "mapping_file",
    "result_file",
    "auto_unroll_results_file",
]


FALLBACK_RE = re.compile(r"fallback_unroll(\d+)$")
ARCH_VEC_WIDTH_RE = re.compile(r"(?:^|[_/-])vec(\d+)(?:[_./-]|$)")


class RepairInterrupted(Exception):
    """Raised when repair shutdown has been requested."""


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    repo_root = default_repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument(
        "--sweep-dir",
        type=Path,
        default=DEFAULT_SWEEP_DIR,
        help="Existing arch sweep directory, relative to --repo-root unless absolute.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Repair output directory, relative to --repo-root unless absolute.",
    )
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--timeout-sec", type=int, default=DEFAULT_TIMEOUT_SEC)
    parser.add_argument("--runmode", default="runall")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--max-jobs",
        type=int,
        default=0,
        help="Limit number of scheduled repair jobs after ordering; 0 means no limit.",
    )
    parser.add_argument(
        "--include-timeouts",
        action="store_true",
        default=True,
        help="Include unrecovered timeout rows after fallback repairs.",
    )
    parser.add_argument(
        "--fallback-only",
        action="store_true",
        help="Only run higher-unroll repairs for rows recovered by fallback_unrollN.",
    )
    parser.add_argument(
        "--no-update-source-results",
        action="store_true",
        help="Do not patch the original experiment results.csv files with better repair rows.",
    )
    return parser.parse_args()


def resolve_under(root: Path, path: Path) -> Path:
    return path if path.is_absolute() else root / path


def read_yaml_mapping(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        data = yaml.safe_load(stream)
    if not isinstance(data, dict):
        raise ValueError(f"Expected top-level mapping in {path}")
    return data


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def write_csv_rows(path: Path, fieldnames: list[str], rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def parse_optional_float(value: Any) -> float | None:
    text = str(value if value is not None else "").strip()
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def parse_optional_int(value: Any) -> int | None:
    parsed = parse_optional_float(value)
    if parsed is None:
        return None
    return int(parsed)


def discover_experiments(sweep_dir: Path) -> list[Experiment]:
    configs_dir = sweep_dir / "configs"
    experiments = []
    for config_path in sorted(configs_dir.glob("*.yaml")):
        name = config_path.stem
        experiments.append(
            Experiment(
                name=name,
                config_path=config_path,
                results_csv=sweep_dir / name / "results.csv",
                config=read_yaml_mapping(config_path),
            )
        )
    return experiments


def kernel_name(row: dict[str, str]) -> str:
    source = row.get("benchmark", "")
    path = Path(source)
    return path.parent.name or path.stem


def auto_unroll_factors(config: dict[str, Any]) -> list[int]:
    auto = config.get("auto_unroll", {})
    factors: list[int] = []
    if isinstance(auto, dict):
        raw_factors = auto.get("factors", [])
        if isinstance(raw_factors, list):
            factors = [int(item) for item in raw_factors]
    if factors:
        return sorted(set(factors))

    arch = str(config.get("arch_template_json", ""))
    match = ARCH_VEC_WIDTH_RE.search(arch)
    if match:
        width = int(match.group(1))
        if width == 4:
            return [4, 8, 16, 32]
        if width == 8:
            return [8, 16, 32]
    return [1, 2, 4, 8]


def fallback_recovery_factor(row: dict[str, str]) -> int | None:
    reason = str(row.get("recovery_reason", "")).strip()
    match = FALLBACK_RE.match(reason)
    return int(match.group(1)) if match else None


def row_is_unrecovered_timeout(row: dict[str, str]) -> bool:
    status = str(row.get("status", "")).strip().lower()
    timeout = str(row.get("timeout", "")).strip().lower()
    recovered = str(row.get("recovered_from_timeout", "")).strip().lower()
    return (status == "timeout" or timeout == "true") and recovered != "true"


def make_repair_config(source: dict[str, Any], unroll_factor: int) -> dict[str, Any]:
    config = dict(source)
    auto = config.get("auto_unroll", {})
    if not isinstance(auto, dict):
        auto = {"enabled": bool(auto)}
    else:
        auto = dict(auto)
    auto["enabled"] = True
    auto["factors"] = [unroll_factor]
    auto["stop_on_degradation"] = True
    config["auto_unroll"] = auto

    config["mapper_preset"] = "fast"
    init_ii = int(config.get("init_II", 1))
    config["max_II"] = max(1, init_ii * 2)
    return config


def build_jobs(experiments: list[Experiment], max_jobs: int, include_timeouts: bool) -> list[RepairJob]:
    fallback_jobs: list[RepairJob] = []
    timeout_jobs: list[RepairJob] = []
    job_index = 1

    for exp in experiments:
        factors = auto_unroll_factors(exp.config)
        for row in read_csv_rows(exp.results_csv):
            fallback_factor = fallback_recovery_factor(row)
            if fallback_factor is not None:
                for factor in factors:
                    if factor > fallback_factor:
                        fallback_jobs.append(
                            RepairJob("fallback_higher_unroll", exp, row, factor, job_index)
                        )
                        job_index += 1

    if include_timeouts:
        for factor_order in sorted({factor for exp in experiments for factor in auto_unroll_factors(exp.config)}):
            for exp in experiments:
                factors = auto_unroll_factors(exp.config)
                if factor_order not in factors:
                    continue
                for row in read_csv_rows(exp.results_csv):
                    if row_is_unrecovered_timeout(row):
                        timeout_jobs.append(
                            RepairJob("timeout_unroll_search", exp, row, factor_order, job_index)
                        )
                        job_index += 1

    jobs = fallback_jobs + timeout_jobs
    if max_jobs > 0:
        jobs = jobs[:max_jobs]
    return [
        RepairJob(job.phase, job.experiment, job.row, job.unroll_factor, idx)
        for idx, job in enumerate(jobs, start=1)
    ]


def repair_tag(job: RepairJob) -> str:
    bench = str(job.row.get("benchmark", "")).replace("/", "__").replace(".c", "")
    pieces = [
        "repair",
        job.phase,
        job.experiment.name,
        f"{job.job_index:04d}",
        bench,
        str(job.row.get("function", "")),
        f"uf{job.unroll_factor}",
    ]
    return sanitize_name("__".join(pieces))


def job_key(job: RepairJob) -> tuple[str, str, str, str, str]:
    return (
        job.phase,
        job.experiment.name,
        job.row.get("benchmark", ""),
        job.row.get("function", ""),
        str(job.unroll_factor),
    )


def result_row_key(row: dict[str, str]) -> tuple[str, str, str, str, str]:
    return (
        row.get("phase", ""),
        row.get("experiment", ""),
        row.get("benchmark", ""),
        row.get("function", ""),
        row.get("repair_unroll", ""),
    )


def repair_job_stem(row: dict[str, str], unroll_factor: int) -> str:
    benchmark = str(row.get("benchmark", "")).replace(".c", "")
    function = str(row.get("function", ""))
    return sanitize_name(f"{benchmark}__{function}__uf{unroll_factor}")


def run_job(
    repo_root: Path,
    output_dir: Path,
    timeout_sec: int,
    runmode: str,
    total: int,
    job: RepairJob,
) -> RepairResult:
    row = job.row
    spec = BenchmarkSpec(str(row.get("benchmark", "")), str(row.get("function", "")))
    run_tag = repair_tag(job)
    kernel = kernel_name(row)
    job_dir = output_dir / job.phase / job.experiment.name / repair_job_stem(row, job.unroll_factor)
    job_dir.mkdir(parents=True, exist_ok=True)
    config_file = job_dir / "repair_config.yaml"
    log_file = job_dir / "log.txt"

    repair_config = make_repair_config(job.experiment.config, job.unroll_factor)
    with config_file.open("w", encoding="utf-8") as stream:
        yaml.safe_dump(repair_config, stream, sort_keys=False)

    mapper_subfolder = read_mapper_subfolder(repo_root, os.path.relpath(config_file, repo_root))
    appfolder = str(Path(spec.rel_source).parent).replace("\\", "/")
    dfg_dir, mapper_dir, sim_dir = workspace_run_dirs(repo_root, run_tag, appfolder, mapper_subfolder)
    for work_dir in (dfg_dir, mapper_dir, sim_dir):
        if work_dir.exists():
            shutil.rmtree(work_dir)

    cmd = [
        sys.executable,
        "-u",
        "run_morpher.py",
        spec.rel_source,
        spec.function,
        os.path.relpath(config_file, repo_root),
        runmode,
        "--run-tag",
        run_tag,
    ]

    start = time.time()
    timed_out = False
    return_code = None
    try:
        completed = run_logged_process(
            cmd,
            cwd=repo_root,
            log_file=log_file,
            timeout_sec=timeout_sec,
            env=os.environ.copy(),
        )
        return_code = completed.returncode
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        with log_file.open("a", encoding="utf-8") as log:
            log.write(f"\n[TIMEOUT] repair command exceeded {timeout_sec}s\n")
            if exc.stdout:
                log.write(str(exc.stdout))
            if exc.stderr:
                log.write(str(exc.stderr))

    metadata = collect_run_metadata(
        log_file=log_file,
        mapper_kernel_dir=mapper_dir,
        sim_kernel_dir=sim_dir,
        start_time=start,
        timed_out=timed_out,
        return_code=return_code,
    )
    elapsed = round(time.time() - start, 2)
    if timed_out:
        status = "timeout"
    elif return_code == 0:
        status = "ok"
    else:
        status = "failed"

    print(
        f"[{job.job_index}/{total}] {job.phase} {kernel} uf={job.unroll_factor} "
        f"status={status} ii={metadata.achieved_ii} score={metadata.auto_unroll_score} "
        f"elapsed={elapsed}s"
    )
    sys.stdout.flush()

    return RepairResult(
        job=job,
        status=status,
        return_code=return_code,
        timeout=timed_out,
        elapsed_sec=elapsed,
        achieved_ii=metadata.achieved_ii,
        score=metadata.auto_unroll_score if metadata.auto_unroll_score is not None else metadata.achieved_ii,
        selected_unroll_factor=metadata.selected_unroll_factor,
        ii_source=metadata.ii_source,
        matches=metadata.matches,
        mismatches=metadata.mismatches,
        run_dir=job_dir,
        log_file=log_file,
        run_tag=run_tag,
        config_file=config_file,
        mapping_file=metadata.mapping_file,
        result_file=metadata.result_file,
        auto_unroll_results_file=metadata.auto_unroll_results_file,
        failure_stage=metadata.failure_stage,
        failure_reason=metadata.failure_reason,
    )


def repair_result_to_source_row(result: RepairResult) -> dict[str, Any]:
    job = result.job
    return {
        "benchmark": job.row.get("benchmark", ""),
        "function": job.row.get("function", ""),
        "status": result.status,
        "return_code": result.return_code,
        "achieved_ii": result.achieved_ii,
        "ii_source": result.ii_source,
        "best_score": result.score,
        "selected_unroll_factor": result.selected_unroll_factor,
        "auto_unroll_score": result.score,
        "matches": result.matches,
        "mismatches": result.mismatches,
        "timeout": result.timeout,
        "recovered_from_timeout": False,
        "recovery_reason": "repair_run",
        "elapsed_sec": result.elapsed_sec,
        "run_dir": str(result.run_dir),
        "log_file": str(result.log_file),
        "run_tag": result.run_tag,
        "failure_stage": result.failure_stage if result.status != "ok" else "",
        "failure_reason": result.failure_reason if result.status != "ok" else "",
        "mapping_file": str(result.mapping_file) if result.mapping_file else "",
        "result_file": str(result.result_file) if result.result_file else "",
        "auto_unroll_results_file": (
            str(result.auto_unroll_results_file) if result.auto_unroll_results_file else ""
        ),
    }


def score_improved(old_row: dict[str, str], new_result: RepairResult) -> tuple[bool, str]:
    if new_result.achieved_ii is None:
        return False, "repair produced no II"
    if new_result.status != "ok" and not new_result.mapping_file:
        return False, f"repair status {new_result.status}"

    old_score = parse_optional_float(old_row.get("best_score", ""))
    new_score = parse_optional_float(new_result.score)

    if new_score is None:
        return False, "repair produced no score"
    if old_score is None:
        return True, "original had no score"
    if new_score is not None and old_score is not None and new_score < old_score:
        return True, f"score {old_score} -> {new_score}"
    return False, "no score improvement"


def update_source_results_csv(result: RepairResult) -> RepairResult:
    job = result.job
    results_csv = job.experiment.results_csv
    rows = read_csv_rows(results_csv)
    key = (job.row.get("benchmark", ""), job.row.get("function", ""))
    updated = False
    update_reason = ""
    new_row = repair_result_to_source_row(result)

    for idx, row in enumerate(rows):
        if (row.get("benchmark", ""), row.get("function", "")) != key:
            continue
        improved, reason = score_improved(row, result)
        update_reason = reason
        if improved:
            backup = results_csv.with_suffix(results_csv.suffix + ".bak")
            if not backup.exists() and results_csv.exists():
                shutil.copy2(results_csv, backup)
            rows[idx] = {field: new_row.get(field, "") for field in CSV_FIELDS}
            write_csv_rows(results_csv, CSV_FIELDS, rows)
            updated = True
        break

    if not update_reason:
        update_reason = "source row not found"
    return RepairResult(
        job=result.job,
        status=result.status,
        return_code=result.return_code,
        timeout=result.timeout,
        elapsed_sec=result.elapsed_sec,
        achieved_ii=result.achieved_ii,
        score=result.score,
        selected_unroll_factor=result.selected_unroll_factor,
        ii_source=result.ii_source,
        matches=result.matches,
        mismatches=result.mismatches,
        run_dir=result.run_dir,
        log_file=result.log_file,
        run_tag=result.run_tag,
        config_file=result.config_file,
        mapping_file=result.mapping_file,
        result_file=result.result_file,
        auto_unroll_results_file=result.auto_unroll_results_file,
        failure_stage=result.failure_stage,
        failure_reason=result.failure_reason,
        source_updated=updated,
        update_reason=update_reason,
    )


def write_repair_kernel_result(result: RepairResult) -> None:
    write_csv_rows(result.run_dir / "result.csv", CSV_FIELDS, [repair_result_to_source_row(result)])


def read_existing_result_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    return read_csv_rows(path)


def result_to_row(result: RepairResult) -> dict[str, Any]:
    job = result.job
    cfg = job.experiment.config
    return {
        "phase": job.phase,
        "experiment": job.experiment.name,
        "benchmark": job.row.get("benchmark", ""),
        "function": job.row.get("function", ""),
        "kernel": kernel_name(job.row),
        "arch": cfg.get("arch_template_json", ""),
        "spm_mode": cfg.get("spm_mode", ""),
        "hops": cfg.get("multihop", ""),
        "original_status": job.row.get("status", ""),
        "original_recovery_reason": job.row.get("recovery_reason", ""),
        "original_unroll": job.row.get("selected_unroll_factor", ""),
        "repair_unroll": job.unroll_factor,
        "status": result.status,
        "return_code": result.return_code,
        "timeout": result.timeout,
        "elapsed_sec": result.elapsed_sec,
        "ii": result.achieved_ii if result.achieved_ii is not None else "",
        "score": result.score if result.score is not None else "",
        "selected_unroll": result.selected_unroll_factor if result.selected_unroll_factor is not None else "",
        "failure_stage": result.failure_stage,
        "failure_reason": result.failure_reason,
        "source_updated": result.source_updated,
        "update_reason": result.update_reason,
        "run_tag": result.run_tag,
        "log_file": str(result.log_file),
        "config_file": str(result.config_file),
        "mapping_file": str(result.mapping_file) if result.mapping_file else "",
        "result_file": str(result.result_file) if result.result_file else "",
        "auto_unroll_results_file": (
            str(result.auto_unroll_results_file) if result.auto_unroll_results_file else ""
        ),
    }


def write_result_rows(rows: list[dict[str, Any]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=RESULT_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in RESULT_FIELDS})


def write_results_csv(
    results: list[RepairResult],
    path: Path,
    existing_rows: list[dict[str, str]] | None = None,
) -> None:
    rows: list[dict[str, Any]] = []
    if existing_rows:
        rows.extend(existing_rows)
    rows.extend(result_to_row(result) for result in results)
    write_result_rows(rows, path)


def shutdown_executor(
    executor: concurrent.futures.ThreadPoolExecutor,
    *,
    wait: bool,
    cancel_futures: bool,
) -> None:
    try:
        executor.shutdown(wait=wait, cancel_futures=cancel_futures)
    except TypeError:
        executor.shutdown(wait=wait)


def record_repair_result(
    result: RepairResult,
    *,
    update_source_results: bool,
    results: list[RepairResult],
    existing_rows: list[dict[str, str]],
    results_csv: Path,
) -> None:
    write_repair_kernel_result(result)
    if update_source_results:
        result = update_source_results_csv(result)
    results.append(result)
    results.sort(key=lambda item: item.job.job_index)
    write_results_csv(results, results_csv, existing_rows)


def run_job_group(
    jobs: list[RepairJob],
    *,
    repo_root: Path,
    output_dir: Path,
    timeout_sec: int,
    runmode: str,
    max_workers: int,
    total_jobs: int,
    update_source_results: bool,
    results: list[RepairResult],
    existing_rows: list[dict[str, str]],
    results_csv: Path,
) -> None:
    if not jobs:
        return
    print(f"\nPhase {jobs[0].phase}: {len(jobs)} repair job(s)")
    completed_futures: set[concurrent.futures.Future[RepairResult]] = set()
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=max_workers)
    futures = [
        pool.submit(
            run_job,
            repo_root,
            output_dir,
            timeout_sec,
            runmode,
            total_jobs,
            job,
        )
        for job in jobs
    ]
    try:
        for future in concurrent.futures.as_completed(futures):
            completed_futures.add(future)
            record_repair_result(
                future.result(),
                update_source_results=update_source_results,
                results=results,
                existing_rows=existing_rows,
                results_csv=results_csv,
            )
    except (KeyboardInterrupt, SystemExit, concurrent.futures.CancelledError):
        for future in futures:
            if future in completed_futures or not future.done() or future.cancelled():
                continue
            try:
                record_repair_result(
                    future.result(),
                    update_source_results=update_source_results,
                    results=results,
                    existing_rows=existing_rows,
                    results_csv=results_csv,
                )
                completed_futures.add(future)
            except BaseException:
                pass
        raise RepairInterrupted("repair run interrupted")
    finally:
        shutdown_executor(
            pool,
            wait=False,
            cancel_futures=True,
        )


def main() -> int:
    install_signal_cleanup()
    args = parse_args()
    if args.jobs <= 0:
        raise ValueError("--jobs must be >= 1")
    repo_root = args.repo_root.resolve()
    sweep_dir = resolve_under(repo_root, args.sweep_dir).resolve()
    output_dir = resolve_under(repo_root, args.output_dir).resolve()
    experiments = discover_experiments(sweep_dir)
    include_timeouts = args.include_timeouts and not args.fallback_only
    jobs = build_jobs(experiments, 0, include_timeouts)
    results_csv = output_dir / "repair_results.csv"
    existing_rows = read_existing_result_rows(results_csv)
    completed_keys = {result_row_key(row) for row in existing_rows}
    total_jobs = len(jobs)
    if completed_keys:
        jobs = [job for job in jobs if job_key(job) not in completed_keys]
    skipped_jobs = total_jobs - len(jobs)
    if args.max_jobs > 0:
        jobs = jobs[: args.max_jobs]

    print(f"Repair jobs remaining: {len(jobs)}")
    if existing_rows:
        print(f"Existing repair rows: {len(existing_rows)}")
        print(f"Skipped completed jobs still in queue: {skipped_jobs}")
    print(f"Output: {output_dir}")
    print(f"Timeout per repair run: {args.timeout_sec}s")
    if args.dry_run:
        for job in jobs:
            row = job.row
            print(
                f"{job.job_index:04d} {job.phase} {job.experiment.name} "
                f"{kernel_name(row)}::{row.get('function', '')} uf={job.unroll_factor}"
            )
        return 0

    results: list[RepairResult] = []
    fallback_jobs = [job for job in jobs if job.phase == "fallback_higher_unroll"]
    timeout_jobs = [job for job in jobs if job.phase == "timeout_unroll_search"]
    try:
        run_job_group(
            fallback_jobs,
            repo_root=repo_root,
            output_dir=output_dir,
            timeout_sec=args.timeout_sec,
            runmode=args.runmode,
            max_workers=args.jobs,
            total_jobs=len(jobs),
            update_source_results=not args.no_update_source_results,
            results=results,
            existing_rows=existing_rows,
            results_csv=results_csv,
        )
        run_job_group(
            timeout_jobs,
            repo_root=repo_root,
            output_dir=output_dir,
            timeout_sec=args.timeout_sec,
            runmode=args.runmode,
            max_workers=args.jobs,
            total_jobs=len(jobs),
            update_source_results=not args.no_update_source_results,
            results=results,
            existing_rows=existing_rows,
            results_csv=results_csv,
        )
    except RepairInterrupted:
        write_results_csv(results, results_csv, existing_rows)
        print(f"\nInterrupted. Wrote {len(results)} completed repair row(s) to {results_csv}")
        return 130

    write_results_csv(results, results_csv, existing_rows)
    print(f"Wrote {len(results)} new repair row(s) to {results_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
