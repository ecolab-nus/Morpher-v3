#!/usr/bin/env python3
"""Sweep synthesized systolic mappings across edge-memory architectures.

This is intentionally a mapper-level sweep, not a DFG-only sweep. The
SystolicCandidatePass can identify candidate loop nests and shape metadata, but
utilization needs a mapped II. The flow is:

  C benchmark -> LLVM IR -> SystolicCandidatePass JSON
  JSON + arch -> scripts/synthesize_systolic.py -> mapper result/util table

Generated outputs live under generated/morpher_runs/systolic_arch_sweep/<tag>.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
BENCH_ROOT = REPO_ROOT / "Morpher_DFG_Generator" / "benchmarks"
PLUGIN = REPO_ROOT / "Morpher_DFG_Generator" / "build" / "src" / "libdfggenPass.so"
SYNTH = REPO_ROOT / "scripts" / "synthesize_systolic.py"
DEFAULT_CONFIG = REPO_ROOT / "config" / "default.yaml"
DEFAULT_OUTPUT = REPO_ROOT / "generated" / "morpher_runs" / "systolic_arch_sweep"

_ACTIVE_PROCESSES: set[subprocess.Popen] = set()
_ACTIVE_LOCK = threading.Lock()
_SHUTDOWN = threading.Event()


@dataclass(frozen=True)
class Benchmark:
    rel_source: str
    function: str


@dataclass(frozen=True)
class SynthTask:
    benchmark: Benchmark
    arch: str
    report: Path
    output_dir: Path


def install_signal_cleanup() -> None:
    def handle_signal(signum: int, _frame: object) -> None:
        _SHUTDOWN.set()
        terminate_active()
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)


def register(process: subprocess.Popen) -> None:
    with _ACTIVE_LOCK:
        _ACTIVE_PROCESSES.add(process)


def unregister(process: subprocess.Popen) -> None:
    with _ACTIVE_LOCK:
        _ACTIVE_PROCESSES.discard(process)


def terminate_active() -> None:
    with _ACTIVE_LOCK:
        processes = list(_ACTIVE_PROCESSES)
    for process in processes:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass


def run_logged(
    cmd: list[str],
    *,
    cwd: Path,
    log_file: Path,
    timeout_sec: int,
) -> tuple[int | None, bool, float]:
    if _SHUTDOWN.is_set():
        raise SystemExit(130)
    log_file.parent.mkdir(parents=True, exist_ok=True)
    timeout = None if timeout_sec <= 0 else timeout_sec
    start = time.time()
    with log_file.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            cmd,
            cwd=cwd,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        register(process)
        try:
            try:
                process.communicate(timeout=timeout)
                return process.returncode, False, time.time() - start
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(os.getpgid(process.pid), signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(os.getpgid(process.pid), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
                return process.returncode, True, time.time() - start
        finally:
            unregister(process)


def discover_benchmarks() -> list[Benchmark]:
    benches: list[Benchmark] = []
    for c_file in sorted(BENCH_ROOT.rglob("*.c")):
        rel = c_file.relative_to(BENCH_ROOT).as_posix()
        rel_path = Path(rel)
        if "__run_" in rel:
            continue
        if len(rel_path.parts) != 3:
            continue
        if rel_path.name != f"{rel_path.parent.name}.c":
            continue
        if not re.fullmatch(r"[A-Za-z_]\w*", rel_path.stem):
            continue
        benches.append(Benchmark(rel, rel_path.stem))
    if not benches:
        raise RuntimeError(f"no benchmarks found under {BENCH_ROOT}")
    return benches


def without_c_suffix(text: str) -> str:
    return text[:-2] if text.endswith(".c") else text


def select_benchmarks(benches: list[Benchmark], args: argparse.Namespace) -> list[Benchmark]:
    selected = benches
    if args.filter:
        regexes = [re.compile(p) for p in args.filter]
        selected = [b for b in selected if any(r.search(b.rel_source) for r in regexes)]
    if args.max_benches > 0:
        selected = selected[: args.max_benches]
    return selected


def discover_arches(args: argparse.Namespace) -> list[str]:
    if args.arch:
        return args.arch
    arch_dir = REPO_ROOT / "Morpher_CGRA_Mapper" / "json_arch" / "edge_mem_4x4"
    arches = []
    for path in sorted(arch_dir.glob("*.json")):
        rel = f"edge_mem_4x4/{path.name}"
        if not args.include_vred and "_vred_" in path.name:
            continue
        arches.append(rel)
    if not arches:
        raise RuntimeError(f"no architecture JSONs found under {arch_dir}")
    return arches


def candidate_report_for(
    bench: Benchmark,
    *,
    output_dir: Path,
    timeout_sec: int,
) -> dict[str, object]:
    stem = without_c_suffix(bench.rel_source.replace("/", "__"))
    work_dir = output_dir / "candidate_reports" / stem
    src = BENCH_ROOT / bench.rel_source
    ll = work_dir / f"{bench.function}.ll"
    report = work_dir / f"{bench.function}.systolic.json"
    compile_log = work_dir / "clang.log"
    pass_log = work_dir / "candidate_pass.log"
    work_dir.mkdir(parents=True, exist_ok=True)
    report.unlink(missing_ok=True)

    clang_cmd = [
        "clang",
        "-O1",
        "-fno-unroll-loops",
        "-emit-llvm",
        "-S",
        str(src),
        "-o",
        str(ll),
    ]
    rc, timed_out, elapsed = run_logged(
        clang_cmd, cwd=REPO_ROOT, log_file=compile_log, timeout_sec=timeout_sec
    )
    if timed_out or rc != 0:
        return {
            "benchmark": bench.rel_source,
            "function": bench.function,
            "stage": "clang",
            "status": "timeout" if timed_out else "failed",
            "report": report,
            "candidate_count": 0,
            "yes_count": 0,
            "maybe_count": 0,
            "elapsed_sec": elapsed,
            "log_file": compile_log,
        }

    opt_cmd = [
        "opt",
        "-load-pass-plugin",
        str(PLUGIN),
        "-passes=morpher-systolic-candidate",
        f"-morpher-systolic-report={report}",
        str(ll),
        "-disable-output",
    ]
    rc, timed_out, elapsed = run_logged(
        opt_cmd, cwd=REPO_ROOT, log_file=pass_log, timeout_sec=timeout_sec
    )
    records = load_json_lines(report)
    yes_count = sum(1 for r in records if r.get("verdict") == "yes")
    maybe_count = sum(1 for r in records if r.get("verdict") == "maybe")
    status = "ok" if rc == 0 and not timed_out else ("timeout" if timed_out else "failed")
    return {
        "benchmark": bench.rel_source,
        "function": bench.function,
        "stage": "candidate_pass",
        "status": status,
        "report": report,
        "candidate_count": len(records),
        "yes_count": yes_count,
        "maybe_count": maybe_count,
        "elapsed_sec": elapsed,
        "log_file": pass_log,
    }


def load_json_lines(path: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    if not path.exists():
        return records
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(obj, dict):
                records.append(obj)
    return records


def count_allowed_verdicts(path: Path, allowed: set[str]) -> int:
    return sum(1 for record in load_json_lines(path) if record.get("verdict") in allowed)


_SYNTH_ROW_RE = re.compile(
    r"^(?P<kernel>\S+)\s+(?P<verdict>yes|maybe|no)\s+"
    r"(?P<template>\S+)\s+(?P<cells>\S+)\s+(?P<nops>\S+)\s+"
    r"(?P<ii>\S+)\s+(?P<map_status>\S+)\s+(?P<util>\S+)\s*$"
)


def parse_synth_rows(log_file: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    if not log_file.exists():
        return rows
    for line in log_file.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = _SYNTH_ROW_RE.match(line.strip())
        if not match:
            continue
        rows.append(match.groupdict())
    return rows


def run_synth_task(
    task: SynthTask,
    *,
    config: Path,
    timeout_sec: int,
    extra_synth_args: list[str],
) -> list[dict[str, object]]:
    arch_stem = Path(task.arch).stem
    bench_stem = without_c_suffix(task.benchmark.rel_source.replace("/", "__"))
    out = task.output_dir / "synth" / arch_stem / bench_stem
    log_file = out / "synthesize.log"
    cmd = [
        sys.executable,
        str(SYNTH),
        "--report",
        str(task.report),
        "--output-dir",
        str(out),
        "--config",
        str(config),
        "--arch",
        task.arch,
        *extra_synth_args,
    ]
    rc, timed_out, elapsed = run_logged(
        cmd, cwd=REPO_ROOT, log_file=log_file, timeout_sec=timeout_sec
    )
    parsed = parse_synth_rows(log_file)
    if not parsed:
        empty_status = "no_allowed_verdicts" if rc == 0 and not timed_out else (
            "timeout" if timed_out else "failed"
        )
        return [
            {
                "benchmark": task.benchmark.rel_source,
                "function": task.benchmark.function,
                "arch": task.arch,
                "kernel": "",
                "verdict": "",
                "template": "",
                "cells": "",
                "nops": "",
                "ii": "",
                "map_status": empty_status,
                "comp_util": "",
                "return_code": rc,
                "timeout": timed_out,
                "elapsed_sec": f"{elapsed:.3f}",
                "log_file": str(log_file),
            }
        ]
    rows: list[dict[str, object]] = []
    for row in parsed:
        rows.append(
            {
                "benchmark": task.benchmark.rel_source,
                "function": task.benchmark.function,
                "arch": task.arch,
                "kernel": row["kernel"],
                "verdict": row["verdict"],
                "template": row["template"],
                "cells": "" if row["cells"] == "-" else row["cells"],
                "nops": "" if row["nops"] == "-" else row["nops"],
                "ii": "" if row["ii"] == "-" else row["ii"],
                "map_status": row["map_status"],
                "comp_util": "" if row["util"] == "-" else row["util"],
                "return_code": rc,
                "timeout": timed_out,
                "elapsed_sec": f"{elapsed:.3f}",
                "log_file": str(log_file),
            }
        )
    return rows


def write_csv(path: Path, rows: list[dict[str, object]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--filter", action="append", default=[])
    parser.add_argument("--max-benches", type=int, default=0)
    parser.add_argument("--arch", action="append", default=[])
    parser.add_argument("--include-vred", action="store_true")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--candidate-timeout-sec", type=int, default=120)
    parser.add_argument("--synth-timeout-sec", type=int, default=900)
    parser.add_argument("--only-verdict", default="yes,maybe")
    parser.add_argument(
        "--dataflow",
        choices=["streaming", "output_stationary", "weight_stationary"],
        default="streaming",
    )
    parser.add_argument(
        "--pin-loads",
        choices=["none", "first", "all"],
        default="none",
    )
    parser.add_argument("--prefer-parallel-matvec", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    install_signal_cleanup()
    args = parse_args()
    if args.jobs <= 0:
        raise ValueError("--jobs must be >= 1")
    if not PLUGIN.is_file():
        raise FileNotFoundError(f"DFG pass plugin not found: {PLUGIN}")

    output_dir = args.output_dir if args.output_dir.is_absolute() else REPO_ROOT / args.output_dir
    config = args.config if args.config.is_absolute() else REPO_ROOT / args.config
    output_dir.mkdir(parents=True, exist_ok=True)

    benches = select_benchmarks(discover_benchmarks(), args)
    arches = discover_arches(args)
    if args.dry_run:
        print(f"benchmarks={len(benches)} arches={len(arches)} output={output_dir}")
        for bench in benches:
            print(f"  bench {bench.rel_source} :: {bench.function}")
        for arch in arches:
            print(f"  arch {arch}")
        return 0

    reports_dir = output_dir / "candidate_reports"
    if reports_dir.exists():
        shutil.rmtree(reports_dir)

    print(f"Running SystolicCandidatePass on {len(benches)} benchmarks...")
    candidate_rows: list[dict[str, object]] = []
    for idx, bench in enumerate(benches, start=1):
        row = candidate_report_for(
            bench,
            output_dir=output_dir,
            timeout_sec=args.candidate_timeout_sec,
        )
        candidate_rows.append(row)
        print(
            f"[candidate {idx}/{len(benches)}] {bench.rel_source}: "
            f"{row['status']} records={row['candidate_count']} "
            f"yes={row['yes_count']} maybe={row['maybe_count']}"
        )
        sys.stdout.flush()

    candidate_csv = output_dir / "candidate_summary.csv"
    write_csv(
        candidate_csv,
        candidate_rows,
        [
            "benchmark",
            "function",
            "stage",
            "status",
            "candidate_count",
            "yes_count",
            "maybe_count",
            "elapsed_sec",
            "report",
            "log_file",
        ],
    )

    report_by_bench = {
        (str(row["benchmark"]), str(row["function"])): Path(row["report"])
        for row in candidate_rows
        if row["status"] == "ok" and int(row["candidate_count"]) > 0
    }
    allowed = {item.strip() for item in args.only_verdict.split(",") if item.strip()}
    report_by_bench = {
        key: report
        for key, report in report_by_bench.items()
        if count_allowed_verdicts(report, allowed) > 0
    }
    tasks = [
        SynthTask(bench, arch, report_by_bench[(bench.rel_source, bench.function)], output_dir)
        for bench in benches
        if (bench.rel_source, bench.function) in report_by_bench
        for arch in arches
    ]
    print(f"Mapping synthesized systolic DFGs for {len(tasks)} benchmark/arch pairs...")
    sys.stdout.flush()

    synth_args = [
        "--only-verdict",
        args.only_verdict,
        "--dataflow",
        args.dataflow,
        "--pin-loads",
        args.pin_loads,
    ]
    if args.prefer_parallel_matvec:
        synth_args.append("--prefer-parallel-matvec")

    result_rows: list[dict[str, object]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        future_to_task = {
            pool.submit(
                run_synth_task,
                task,
                config=config,
                timeout_sec=args.synth_timeout_sec,
                extra_synth_args=synth_args,
            ): task
            for task in tasks
        }
        for idx, future in enumerate(concurrent.futures.as_completed(future_to_task), start=1):
            task = future_to_task[future]
            rows = future.result()
            result_rows.extend(rows)
            ok = sum(1 for row in rows if row.get("map_status") == "success")
            print(
                f"[map {idx}/{len(tasks)}] {task.benchmark.rel_source} "
                f"{task.arch}: rows={len(rows)} success={ok}"
            )
            sys.stdout.flush()

    results_csv = output_dir / "systolic_arch_results.csv"
    write_csv(
        results_csv,
        result_rows,
        [
            "benchmark",
            "function",
            "arch",
            "kernel",
            "verdict",
            "template",
            "cells",
            "nops",
            "ii",
            "map_status",
            "comp_util",
            "return_code",
            "timeout",
            "elapsed_sec",
            "log_file",
        ],
    )

    total_success = sum(1 for row in result_rows if row.get("map_status") == "success")
    print(f"Candidate CSV: {candidate_csv}")
    print(f"Results CSV: {results_csv}")
    print(f"Mapped rows: {len(result_rows)} success={total_success}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
