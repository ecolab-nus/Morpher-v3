#!/usr/bin/env python3
"""Generate benchmark operation summaries from the real Morpher DFG pipeline.

This script runs `morpher_runner.dfg_generator.run_dfg_generator()` over the
discovered Morpher benchmarks and reports counts from the staged mapper XML.
It intentionally disables simulation so the summary does not depend on trace
generation or 32-bit benchmark execution.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import csv
import io
import json
import os
import re
import shutil
import signal
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
from morpher_runner.exec import terminate_active_process_groups

BENCH_ROOT = REPO_ROOT / "Morpher_DFG_Generator" / "benchmarks"
DEFAULT_CONFIG = REPO_ROOT / "config" / "default.yaml"
DEFAULT_OUTPUT = REPO_ROOT / "benchmark_ops.csv"
DEFAULT_VECTORIZATIONS = "1,4,8"
VECTOR_TARGET_ARCHES = {
    1: "edge_mem_4x4/scalar_4side.json",
    4: "edge_mem_4x4/vec4_4side.json",
    8: "edge_mem_4x4/vec8_4side.json",
}
DEFAULT_ADDRGEN_MODES = "keep,no_agi,no_iter,pruned"
MEMORY_OP_PREFIXES = (
    "LOAD",
    "STORE",
    "OLOAD",
    "OSTORE",
    "VLOAD",
    "VSTORE",
    "VOLOAD",
    "VOSTORE",
)
SUMMARY_RE = re.compile(
    r"Staged DFG summary:\s*total_nodes=(?P<total>\d+),\s*"
    r"scalar_compute=(?P<scalar>\d+),\s*vector_compute=(?P<vector>\d+),\s*"
    r"memory_ops=(?P<memory>\d+)"
)
AUTO_UNROLL_HEADER_RE = re.compile(r"==\s*Auto-unroll candidate factor\s+(\d+)\s*==")


@dataclass(frozen=True)
class BenchmarkSpec:
    rel_source: str
    function: str


@dataclass
class DfgRow:
    benchmark: str
    function: str
    suite: str
    kernel: str
    vectorization: str
    addrgen_mode: str
    remove_agi: bool
    remove_iter: bool
    unroll: int
    status: str
    total_nodes: int | str = ""
    scalar_compute: int | str = ""
    vector_compute: int | str = ""
    memory_ops: int | str = ""
    compute_ops: int | str = ""
    elapsed_sec: str = ""
    run_tag: str = ""
    dfg_xml: str = ""
    mem_alloc: str = ""
    log_file: str = ""
    failure_reason: str = ""


class JobTimeoutError(TimeoutError):
    pass


@dataclass(frozen=True)
class Variant:
    arch: str
    addrgen_mode: str
    remove_agi: bool
    remove_iter: bool

    @property
    def vectorization(self) -> str:
        match = re.search(r"(?:^|[_/-])vec(\d+)(?:[_./-]|$)", self.arch)
        return match.group(1) if match else "1"

    @property
    def label(self) -> str:
        return f"vec{self.vectorization}__{self.addrgen_mode}"


def parse_log_dfg_summary(log_path: Path, unroll_factor: int | str | None = None) -> dict | None:
    """Extract a staged DFG summary from a Morpher log.

    Retained for scripts that summarize full benchmark/sweep logs. When an
    auto-unroll factor is supplied, the matching candidate block is preferred;
    otherwise the last staged summary in the log is returned.
    """
    try:
        text = log_path.read_text(errors="replace")
    except OSError:
        return None

    headers = list(AUTO_UNROLL_HEADER_RE.finditer(text))
    if headers:
        blocks: list[tuple[int | None, str]] = []
        for idx, match in enumerate(headers):
            end = headers[idx + 1].start() if idx + 1 < len(headers) else len(text)
            blocks.append((int(match.group(1)), text[match.end() : end]))
    else:
        blocks = [(None, text)]

    requested: int | None = None
    if unroll_factor not in (None, "", "0"):
        try:
            requested = int(unroll_factor)
        except (TypeError, ValueError):
            requested = None

    def last_summary(block_text: str) -> dict | None:
        matches = list(SUMMARY_RE.finditer(block_text))
        if not matches:
            return None
        match = matches[-1]
        return {
            "scalar_compute": int(match.group("scalar")),
            "vector_compute": int(match.group("vector")),
            "memory_ops": int(match.group("memory")),
            "total_nodes": int(match.group("total")),
        }

    if requested is not None:
        for factor, block in blocks:
            if factor == requested:
                summary = last_summary(block)
                if summary is not None:
                    summary["unroll_factor"] = factor
                    summary["unroll_factor_matched"] = True
                    return summary

    for factor, block in reversed(blocks):
        summary = last_summary(block)
        if summary is not None:
            summary["unroll_factor"] = factor
            summary["unroll_factor_matched"] = False if requested is not None else None
            return summary
    return None


def repo_relative(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def discover_benchmarks(bench_root: Path) -> list[BenchmarkSpec]:
    specs: list[BenchmarkSpec] = []
    invalid: list[str] = []
    for c_file in sorted(bench_root.rglob("*.c")):
        rel = c_file.relative_to(bench_root)
        if "__run_" in rel.as_posix():
            continue
        if len(rel.parts) != 3 or rel.name != f"{rel.parent.name}.c":
            invalid.append(rel.as_posix())
            continue
        function = rel.stem
        if not re.fullmatch(r"[A-Za-z_]\w*", function):
            invalid.append(rel.as_posix())
            continue
        specs.append(BenchmarkSpec(rel.as_posix(), function))
    if invalid:
        shown = "\n  - ".join(invalid[:40])
        extra = "" if len(invalid) <= 40 else f"\n  ... and {len(invalid) - 40} more"
        print(
            "WARNING: skipping benchmark .c files outside '<suite>/<kernel>/<kernel>.c':\n"
            f"  - {shown}{extra}",
            file=sys.stderr,
        )
    if not specs:
        raise RuntimeError(f"No benchmarks found under {bench_root}")
    return specs


def filter_benchmarks(
    specs: list[BenchmarkSpec], filters: list[str], max_benches: int
) -> list[BenchmarkSpec]:
    selected = specs
    if filters:
        regexes = [re.compile(pattern) for pattern in filters]
        selected = [
            spec for spec in selected if any(regex.search(spec.rel_source) for regex in regexes)
        ]
    if max_benches > 0:
        selected = selected[:max_benches]
    return selected


def read_yaml(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        data = yaml.safe_load(stream)
    if not isinstance(data, dict):
        raise RuntimeError(f"Config must contain a top-level mapping: {path}")
    return data


def write_summary_config(
    source_config: Path,
    output_config: Path,
    *,
    arch: str,
    unroll: int,
    remove_agi: bool,
    remove_iter: bool,
) -> None:
    config = read_yaml(source_config)
    config["arch_template_json"] = arch
    config["enable_simulation"] = False
    config["generate_dfg_pdf"] = False
    config["suppress_dfg_dot"] = True
    config["remove_agi"] = remove_agi
    config["remove_iter"] = remove_iter
    auto = config.get("auto_unroll", {})
    if not isinstance(auto, dict):
        auto = {"enabled": bool(auto)}
    else:
        auto = dict(auto)
    auto["enabled"] = False
    auto["factors"] = [unroll]
    config["auto_unroll"] = auto
    output_config.parent.mkdir(parents=True, exist_ok=True)
    with output_config.open("w", encoding="utf-8") as stream:
        yaml.safe_dump(config, stream, sort_keys=False)


def classify_xml(xml_path: Path) -> dict[str, int]:
    text = xml_path.read_text(encoding="utf-8")
    root = ET.fromstring(f"<Root>\n{text}\n</Root>")
    dfg = root.find("DFG")
    if dfg is None:
        raise RuntimeError(f"DFG XML missing <DFG> root: {xml_path}")
    counts = {
        "total_nodes": 0,
        "scalar_compute": 0,
        "vector_compute": 0,
        "memory_ops": 0,
    }
    for node in dfg.findall("Node"):
        counts["total_nodes"] += 1
        op_elem = node.find("OP")
        op = (op_elem.text or "").strip() if op_elem is not None and op_elem.text else ""
        if op.startswith(MEMORY_OP_PREFIXES):
            counts["memory_ops"] += 1
        elif op.startswith("V"):
            counts["vector_compute"] += 1
        else:
            counts["scalar_compute"] += 1
    return counts


def load_runner_modules() -> tuple[Any, Any]:
    if str(REPO_ROOT) not in sys.path:
        sys.path.insert(0, str(REPO_ROOT))
    from morpher_runner.context import build_context
    from morpher_runner.dfg_generator import run_dfg_generator

    return build_context, run_dfg_generator


def run_one(
    spec: BenchmarkSpec,
    *,
    idx: int,
    total: int,
    variant: Variant,
    unroll: int,
    config_path: Path,
    run_tag_prefix: str,
    log_dir: Path,
    timeout_sec: int,
) -> DfgRow:
    build_context, run_dfg_generator = load_runner_modules()
    rel_path = Path(spec.rel_source)
    suite, kernel = rel_path.parts[0], rel_path.parts[1]
    run_tag = f"{run_tag_prefix}__{idx:03d}_{kernel}__{variant.label}__uf{unroll}"
    csource = spec.rel_source
    log_dir.mkdir(parents=True, exist_ok=True)
    log_file = log_dir / f"{idx:03d}_{suite}__{kernel}__{variant.label}__uf{unroll}.log"
    start = time.monotonic()
    row = DfgRow(
        benchmark=spec.rel_source,
        function=spec.function,
        suite=suite,
        kernel=kernel,
        vectorization=variant.vectorization,
        addrgen_mode=variant.addrgen_mode,
        remove_agi=variant.remove_agi,
        remove_iter=variant.remove_iter,
        unroll=unroll,
        status="failed",
        run_tag=run_tag,
        log_file=repo_relative(log_file),
    )
    try:
        def handle_timeout(_signum: int, _frame: object) -> None:
            terminate_active_process_groups()
            raise JobTimeoutError(f"timed out after {timeout_sec} seconds")

        if timeout_sec > 0:
            signal.signal(signal.SIGALRM, handle_timeout)
            signal.alarm(timeout_sec)
        context = build_context(csource, spec.function, str(config_path), "dfg_gen_only", run_tag)
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer), contextlib.redirect_stderr(buffer):
            run_dfg_generator(context, unroll_factor=unroll)
        if timeout_sec > 0:
            signal.alarm(0)
        log_file.write_text(buffer.getvalue(), encoding="utf-8")
        xml_path = Path(context.mapper_kernel) / f"{spec.function}_PartPredDFG.xml"
        mem_alloc = Path(context.mapper_kernel) / f"{spec.function}_mem_alloc.txt"
        counts = classify_xml(xml_path)
        row.status = "ok"
        row.total_nodes = counts["total_nodes"]
        row.scalar_compute = counts["scalar_compute"]
        row.vector_compute = counts["vector_compute"]
        row.memory_ops = counts["memory_ops"]
        row.compute_ops = counts["scalar_compute"] + counts["vector_compute"]
        row.dfg_xml = repo_relative(xml_path)
        row.mem_alloc = repo_relative(mem_alloc) if mem_alloc.exists() else ""
    except Exception as exc:  # noqa: BLE001 - each row records pipeline failures.
        if isinstance(exc, JobTimeoutError):
            row.status = "timeout"
        with log_file.open("a", encoding="utf-8") as stream:
            stream.write(f"\n[benchmark_op_summary failure] {type(exc).__name__}: {exc}\n")
        row.failure_reason = f"{type(exc).__name__}: {exc}"
    finally:
        if timeout_sec > 0:
            signal.alarm(0)
        row.elapsed_sec = f"{time.monotonic() - start:.3f}"
    print(
        f"[{idx}/{total}] {spec.rel_source} vec{variant.vectorization} "
        f"{variant.addrgen_mode} uf{unroll}: {row.status}",
        flush=True,
    )
    return row


def run_job(
    spec: BenchmarkSpec,
    variant: Variant,
    unroll: int,
    *,
    idx: int,
    total: int,
    source_config: Path,
    temp_dir: Path,
    run_tag_prefix: str,
    log_dir: Path,
    keep_workspace: bool,
    timeout_sec: int,
) -> tuple[int, DfgRow]:
    temp_config = temp_dir / f"config_{idx:05d}.yaml"
    write_summary_config(
        source_config,
        temp_config,
        arch=variant.arch,
        unroll=unroll,
        remove_agi=variant.remove_agi,
        remove_iter=variant.remove_iter,
    )
    row = run_one(
        spec,
        idx=idx,
        total=total,
        variant=variant,
        unroll=unroll,
        config_path=temp_config,
        run_tag_prefix=run_tag_prefix,
        log_dir=log_dir,
        timeout_sec=timeout_sec,
    )
    if not keep_workspace:
        shutil.rmtree(REPO_ROOT / "workspace" / "morpher_runs" / row.run_tag, ignore_errors=True)
    return idx, row


FIELDS = [
    "kernel",
    "vectorization",
    "addrgen_mode",
    "remove_agi",
    "remove_iter",
    "unroll",
    "status",
    "total_nodes",
    "scalar_compute",
    "vector_compute",
    "memory_ops",
    "compute_ops",
]


def row_dict(row: DfgRow) -> dict[str, Any]:
    return {field: getattr(row, field) for field in FIELDS}


def best_rows(rows: list[DfgRow]) -> list[DfgRow]:
    grouped: dict[tuple[str, str, str], list[DfgRow]] = {}
    for row in rows:
        grouped.setdefault((row.benchmark, row.vectorization, row.addrgen_mode), []).append(row)
    chosen: list[DfgRow] = []
    for key in sorted(grouped):
        candidates = [row for row in grouped[key] if row.status == "ok"]
        if not candidates:
            chosen.append(grouped[key][0])
            continue
        chosen.append(min(candidates, key=lambda row: (int(row.total_nodes), row.unroll)))
    return chosen


def write_output(rows: list[DfgRow], output: Path | None, fmt: str) -> None:
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        sink = output.open("w", encoding="utf-8", newline="")
    else:
        sink = sys.stdout
    try:
        if fmt == "csv":
            writer = csv.DictWriter(sink, fieldnames=FIELDS)
            writer.writeheader()
            for row in rows:
                writer.writerow(row_dict(row))
        elif fmt == "json":
            json.dump([row_dict(row) for row in rows], sink, indent=2)
            sink.write("\n")
        else:
            sink.write("| " + " | ".join(FIELDS) + " |\n")
            sink.write("| " + " | ".join(["---"] * len(FIELDS)) + " |\n")
            for row in rows:
                values = [str(row_dict(row)[field]).replace("|", "\\|") for field in FIELDS]
                sink.write("| " + " | ".join(values) + " |\n")
    finally:
        if output is not None:
            sink.close()


def backup_incompatible_output(output: Path, fmt: str) -> None:
    if fmt != "csv" or not output.exists():
        return
    try:
        with output.open("r", encoding="utf-8", newline="") as stream:
            reader = csv.reader(stream)
            header = next(reader, [])
    except OSError:
        return
    if not header or header == FIELDS:
        return
    backup = output.with_suffix(output.suffix + ".old_schema")
    counter = 1
    while backup.exists():
        backup = output.with_suffix(output.suffix + f".old_schema.{counter}")
        counter += 1
    output.rename(backup)
    print(f"Moved incompatible existing output to {repo_relative(backup)}", file=sys.stderr)


def parse_unrolls(raw: str) -> list[int]:
    values: list[int] = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        value = int(item)
        if value < 1:
            raise ValueError("unroll factors must be >= 1")
        if value not in values:
            values.append(value)
    if not values:
        raise ValueError("at least one unroll factor is required")
    return values


def parse_csv_list(raw: str) -> list[str]:
    values: list[str] = []
    for item in raw.split(","):
        item = item.strip()
        if item and item not in values:
            values.append(item)
    if not values:
        raise ValueError("expected at least one comma-separated value")
    return values


def sanitize_token(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("_")


def addrgen_flags(mode: str) -> tuple[bool, bool]:
    mode = mode.strip().lower()
    if mode in ("keep", "with", "with_agi_iter", "full"):
        return False, False
    if mode in ("no_agi", "without_agi"):
        return True, False
    if mode in ("no_iter", "without_iter"):
        return False, True
    if mode in ("pruned", "no_agi_iter", "without", "without_agi_iter"):
        return True, True
    raise ValueError("unknown addrgen mode %r; use keep,no_agi,no_iter,pruned" % mode)


def parse_vectorizations(raw: str) -> list[int]:
    values: list[int] = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        value = int(item)
        if value not in VECTOR_TARGET_ARCHES:
            supported = ",".join(str(key) for key in sorted(VECTOR_TARGET_ARCHES))
            raise ValueError(f"unsupported vectorization {value}; supported values: {supported}")
        if value not in values:
            values.append(value)
    if not values:
        raise ValueError("at least one vectorization target is required")
    return values


def build_variants(vectorizations: list[int], modes: list[str]) -> list[Variant]:
    variants: list[Variant] = []
    for vectorization in vectorizations:
        for mode in modes:
            remove_agi, remove_iter = addrgen_flags(mode)
            variants.append(
                Variant(
                    arch=VECTOR_TARGET_ARCHES[vectorization],
                    addrgen_mode=mode.strip().lower(),
                    remove_agi=remove_agi,
                    remove_iter=remove_iter,
                )
            )
    return variants


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench-root", type=Path, default=BENCH_ROOT)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--unrolls", default="1,2,4,8", help="Comma-separated factors to sweep.")
    parser.add_argument(
        "--vectorizations",
        default=DEFAULT_VECTORIZATIONS,
        help="Comma-separated vectorization targets to sweep. Supported: 1,4,8.",
    )
    parser.add_argument(
        "--addrgen-modes",
        default=DEFAULT_ADDRGEN_MODES,
        help="Comma-separated modes from keep,no_agi,no_iter,pruned.",
    )
    parser.add_argument("--filter", action="append", default=[], help="Regex on benchmark source path.")
    parser.add_argument("--max-benches", type=int, default=0)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--format", choices=("csv", "json", "markdown"), default="csv")
    parser.add_argument("--best-only", action="store_true", help="Emit the smallest successful DFG per benchmark.")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--jobs", type=int, default=1, help="Parallel DFG-generation jobs.")
    parser.add_argument(
        "--timeout-sec",
        type=int,
        default=0,
        help="Per DFG-generation job timeout in seconds (0 = no timeout).",
    )
    parser.add_argument("--run-tag-prefix", default="op_summary")
    parser.add_argument("--log-dir", type=Path, default=REPO_ROOT / "generated" / "benchmark_op_summary_logs")
    parser.add_argument(
        "--keep-workspace",
        action="store_true",
        help="Keep workspace/morpher_runs/<run-tag> directories after counting XML nodes.",
    )
    args = parser.parse_args(argv)
    if args.jobs < 1:
        raise ValueError("--jobs must be >= 1")
    if args.timeout_sec < 0:
        raise ValueError("--timeout-sec must be >= 0")

    os.chdir(REPO_ROOT)
    bench_root = args.bench_root.resolve()
    source_config = args.config.resolve()
    if not source_config.exists():
        raise FileNotFoundError(source_config)
    unrolls = parse_unrolls(args.unrolls)
    variants = build_variants(
        parse_vectorizations(args.vectorizations),
        parse_csv_list(args.addrgen_modes),
    )
    selected = filter_benchmarks(discover_benchmarks(bench_root), args.filter, args.max_benches)
    if args.dry_run:
        print(
            f"{len(selected)} benchmarks x {len(variants)} variants x {len(unrolls)} unrolls "
            f"= {len(selected) * len(variants) * len(unrolls)} DFG-generation jobs"
        )
        print("Variants:")
        for variant in variants:
            print(
                f"  vectorization={variant.vectorization} addrgen_mode={variant.addrgen_mode} "
                f"remove_agi={int(variant.remove_agi)} remove_iter={int(variant.remove_iter)}"
            )
        print("Benchmarks:")
        for spec in selected:
            print(f"{spec.rel_source} :: {spec.function}")
        return 0

    output_path = args.output.resolve() if args.output else None
    if output_path is not None:
        backup_incompatible_output(output_path, args.format)

    jobs = [
        (spec, variant, unroll)
        for spec in selected
        for variant in variants
        for unroll in unrolls
    ]
    total = len(jobs)
    indexed_rows: dict[int, DfgRow] = {}
    with tempfile.TemporaryDirectory(prefix="benchmark_op_summary_") as temp_dir:
        temp_root = Path(temp_dir)
        log_dir = args.log_dir.resolve()
        if args.jobs == 1:
            for idx, (spec, variant, unroll) in enumerate(jobs, start=1):
                row_idx, row = run_job(
                    spec,
                    variant,
                    unroll,
                    idx=idx,
                    total=total,
                    source_config=source_config,
                    temp_dir=temp_root,
                    run_tag_prefix=args.run_tag_prefix,
                    log_dir=log_dir,
                    keep_workspace=args.keep_workspace,
                    timeout_sec=args.timeout_sec,
                )
                indexed_rows[row_idx] = row
        else:
            with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as executor:
                futures = [
                    executor.submit(
                        run_job,
                        spec,
                        variant,
                        unroll,
                        idx=idx,
                        total=total,
                        source_config=source_config,
                        temp_dir=temp_root,
                        run_tag_prefix=args.run_tag_prefix,
                        log_dir=log_dir,
                        keep_workspace=args.keep_workspace,
                        timeout_sec=args.timeout_sec,
                    )
                    for idx, (spec, variant, unroll) in enumerate(jobs, start=1)
                ]
                for future in concurrent.futures.as_completed(futures):
                    row_idx, row = future.result()
                    indexed_rows[row_idx] = row

    rows = [indexed_rows[idx] for idx in sorted(indexed_rows)]
    output_rows = best_rows(rows) if args.best_only else rows
    write_output(output_rows, output_path, args.format)
    return 1 if any(row.status != "ok" for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
