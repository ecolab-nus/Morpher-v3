#!/usr/bin/env python3
"""Build a compact CSV summary for architecture sweep results."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from benchmark_op_summary import parse_log_dfg_summary  # noqa: E402


RESULT_FIELDS = [
    "benchmark",
    "function",
    "status",
    "return_code",
    "achieved_ii",
    "ii_source",
    "best_score",
    "selected_unroll_factor",
    "auto_unroll_score",
    "matches",
    "mismatches",
    "timeout",
    "recovered_from_timeout",
    "recovery_reason",
    "elapsed_sec",
    "run_dir",
    "log_file",
    "run_tag",
    "failure_stage",
    "failure_reason",
    "mapping_file",
    "result_file",
    "auto_unroll_results_file",
]

SUMMARY_FIELDS = [
    "kernel",
    "vectorization",
    "edge_sides",
    "unidirectional",
    "spm_mode",
    "hops",
    "addrgen",
    "ii",
    "unroll",
    "score",
    "scalar_compute",
    "vector_compute",
    "memory_ops",
    "dfg_total_nodes",
    "dfg_matches_unroll",
    "elapsed_sec",
]

PATH_FIELDS = [
    "benchmark",
    "function",
    "experiment",
    "arch",
    "status",
    "ii_source",
    "timeout",
    "recovered",
    "recovery_reason",
    "failure_stage",
    "failure_reason",
    "agu",
    "iter",
    "layout",
    "replicate_data",
    "preset",
    "results_csv",
    "row_source_csv",
    "log_file",
    "mapping_file",
    "result_file",
    "auto_unroll_results_file",
    "run_tag",
]

BMARK_ROOT = Path("Morpher_DFG_Generator") / "benchmarks"


@dataclass(frozen=True)
class ExperimentConfig:
    name: str
    config_path: Path
    results_csv: Path
    config: dict[str, Any]


@dataclass(frozen=True)
class RowCandidate:
    experiment: ExperimentConfig
    row: dict[str, str]
    source_csv: Path
    source_mtime: float


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    repo_root = default_repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument(
        "--sweep-dir",
        type=Path,
        default=Path("generated/morpher_runs/arch_sweeps/edge_mem_arch_sweep"),
        help="Sweep output directory, relative to --repo-root unless absolute.",
    )
    parser.add_argument(
        "--generated-root",
        type=Path,
        default=Path("generated"),
        help="Per-kernel result root, relative to --repo-root unless absolute.",
    )
    parser.add_argument(
        "--output-csv",
        type=Path,
        default=None,
        help="Output CSV path. Defaults to <sweep-dir>/kernel_variation_summary.csv.",
    )
    parser.add_argument(
        "--include-paths",
        action="store_true",
        help="Append debug provenance/path columns.",
    )
    parser.add_argument(
        "--include-kernel-result-csvs",
        action="store_true",
        help=(
            "Also scan per-kernel generated/*/*/*/result.csv files. "
            "This is slower and is only needed if an experiment results.csv is stale."
        ),
    )
    return parser.parse_args()


def resolve_under(root: Path, path: Path) -> Path:
    return path if path.is_absolute() else root / path


def read_yaml_mapping(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        data = yaml.safe_load(stream)
    if not isinstance(data, dict):
        raise ValueError(f"Expected top-level YAML mapping in {path}")
    return data


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def discover_experiments(sweep_dir: Path) -> list[ExperimentConfig]:
    configs_dir = sweep_dir / "configs"
    experiments: list[ExperimentConfig] = []
    for config_path in sorted(configs_dir.glob("*.yaml")):
        name = config_path.stem
        experiments.append(
            ExperimentConfig(
                name=name,
                config_path=config_path,
                results_csv=sweep_dir / name / "results.csv",
                config=read_yaml_mapping(config_path),
            )
        )
    return experiments


def result_key(row: dict[str, str]) -> tuple[str, str]:
    return row.get("benchmark", ""), row.get("function", "")


def experiment_prefix(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("_")


def read_experiment_candidates(exp: ExperimentConfig) -> list[RowCandidate]:
    try:
        mtime = exp.results_csv.stat().st_mtime
    except OSError:
        mtime = 0.0
    return [
        RowCandidate(exp, row, exp.results_csv, mtime)
        for row in read_csv_rows(exp.results_csv)
    ]


def read_kernel_candidates(
    repo_root: Path,
    generated_root: Path,
    experiments: list[ExperimentConfig],
) -> list[RowCandidate]:
    by_prefix = {experiment_prefix(exp.name): exp for exp in experiments}
    candidates: list[RowCandidate] = []
    benchmark_dirs = []
    for source_path in (repo_root / BMARK_ROOT).glob("*/*/*.c"):
        suite = source_path.parent.parent.name
        kernel = source_path.parent.name
        benchmark_dirs.append(generated_root / suite / kernel)

    for benchmark_dir in sorted(set(benchmark_dirs)):
        if not benchmark_dir.exists():
            continue
        result_csvs = benchmark_dir.glob("*/result.csv")
        for result_csv in result_csvs:
            try:
                mtime = result_csv.stat().st_mtime
                rows = read_csv_rows(result_csv)
            except OSError:
                continue
            for row in rows:
                run_tag = row.get("run_tag", "")
                matched_exp = None
                for prefix, exp in by_prefix.items():
                    if run_tag == prefix or run_tag.startswith(prefix + "__"):
                        matched_exp = exp
                        break
                if matched_exp is not None:
                    candidates.append(RowCandidate(matched_exp, row, result_csv, mtime))
    return candidates


def infer_vectorization(arch: str) -> str:
    name = Path(arch).stem
    match = re.search(r"(?:^|_)vec(\d+)(?:_|$)", name)
    if match:
        return match.group(1)
    return "1"


def infer_edge_sides(arch: str) -> str:
    match = re.search(r"_(\d+)side", Path(arch).stem)
    return match.group(1) if match else ""


def bool_int(value: Any) -> int:
    return int(bool(value))


def kernel_name(row: dict[str, str]) -> str:
    source = row.get("benchmark", "")
    path = Path(source)
    return path.parent.name or path.stem


def compact_status(row: dict[str, str], field: str) -> str:
    value = row.get(field, "")
    return "" if value is None else str(value)


def compact_numeric(row: dict[str, str], field: str) -> str:
    value = compact_status(row, field).strip()
    return value if value else "0"


def summarize_candidate(candidate: RowCandidate) -> dict[str, str]:
    exp = candidate.experiment
    cfg = exp.config
    row = candidate.row
    arch = str(cfg.get("arch_template_json", ""))
    pipeline = cfg.get("dfg_pipeline", {}) or {}
    mode = pipeline.get("externalize_access", "off") if isinstance(pipeline, dict) else "off"
    agu = int(mode == "off")
    iterator = int(mode != "agi_iter")
    addrgen = str(agu) if agu == iterator else f"agu{agu}_iter{iterator}"

    log_path = compact_status(row, "log_file")
    dfg = None
    if log_path:
        dfg = parse_log_dfg_summary(
            Path(log_path),
            row.get("selected_unroll_factor"),
        )
    if dfg is None:
        dfg = {
            "scalar_compute": "",
            "vector_compute": "",
            "memory_ops": "",
            "total_nodes": "",
            "unroll_factor_matched": "",
        }
    matched = dfg.get("unroll_factor_matched")
    matched_str = "" if matched is None else str(bool(matched))

    summary = {
        "kernel": kernel_name(row),
        "benchmark": compact_status(row, "benchmark"),
        "function": compact_status(row, "function"),
        "experiment": exp.name,
        "arch": arch,
        "vectorization": infer_vectorization(arch),
        "edge_sides": infer_edge_sides(arch),
        "unidirectional": str(bool_int("_unidir" in Path(arch).stem)),
        "spm_mode": str(cfg.get("spm_mode", "")),
        "hops": str(cfg.get("multihop", "")),
        "addrgen": addrgen,
        "ii": compact_numeric(row, "achieved_ii"),
        "unroll": compact_numeric(row, "selected_unroll_factor"),
        "score": compact_numeric(row, "best_score"),
        "scalar_compute": str(dfg["scalar_compute"]),
        "vector_compute": str(dfg["vector_compute"]),
        "memory_ops":     str(dfg["memory_ops"]),
        "dfg_total_nodes": str(dfg["total_nodes"]),
        "dfg_matches_unroll": matched_str,
        "elapsed_sec": compact_status(row, "elapsed_sec"),
        "status": compact_status(row, "status"),
        "ii_source": compact_status(row, "ii_source"),
        "timeout": compact_status(row, "timeout"),
        "recovered": compact_status(row, "recovered_from_timeout"),
        "recovery_reason": compact_status(row, "recovery_reason"),
        "failure_stage": compact_status(row, "failure_stage"),
        "failure_reason": compact_status(row, "failure_reason"),
        "agu": str(agu),
        "iter": str(iterator),
        "layout": str(bool_int(cfg.get("layout_solver", False))),
        "replicate_data": str(bool_int(cfg.get("replicate_data", False))),
        "preset": str(cfg.get("mapper_preset", "")),
        "results_csv": str(exp.results_csv),
        "row_source_csv": str(candidate.source_csv),
        "log_file": compact_status(row, "log_file"),
        "mapping_file": compact_status(row, "mapping_file"),
        "result_file": compact_status(row, "result_file"),
        "auto_unroll_results_file": compact_status(row, "auto_unroll_results_file"),
        "run_tag": compact_status(row, "run_tag"),
    }
    return summary


def sort_key(row: dict[str, str]) -> tuple[Any, ...]:
    def int_or_large(value: str) -> int:
        try:
            return int(value)
        except (TypeError, ValueError):
            return 10**9

    return (
        row["kernel"],
        row["function"],
        row["benchmark"],
        int_or_large(row["vectorization"]),
        int_or_large(row["hops"]),
        row["spm_mode"],
        int_or_large(row["edge_sides"]),
        row["unidirectional"],
        row["addrgen"],
        row["experiment"],
    )


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    sweep_dir = resolve_under(repo_root, args.sweep_dir).resolve()
    generated_root = resolve_under(repo_root, args.generated_root).resolve()
    output_csv = (
        resolve_under(repo_root, args.output_csv).resolve()
        if args.output_csv is not None
        else sweep_dir / "kernel_variation_summary.csv"
    )

    experiments = discover_experiments(sweep_dir)
    if not experiments:
        raise FileNotFoundError(f"No generated configs found under {sweep_dir / 'configs'}")

    best: dict[tuple[str, str, str], RowCandidate] = {}
    candidates = []
    for exp in experiments:
        candidates.extend(read_experiment_candidates(exp))
    if args.include_kernel_result_csvs:
        candidates.extend(read_kernel_candidates(repo_root, generated_root, experiments))

    for candidate in candidates:
        key = (candidate.experiment.name, *result_key(candidate.row))
        previous = best.get(key)
        if previous is None or candidate.source_mtime >= previous.source_mtime:
            best[key] = candidate

    rows = [summarize_candidate(candidate) for candidate in best.values()]
    rows.sort(key=sort_key)

    fields = SUMMARY_FIELDS + (PATH_FIELDS if args.include_paths else [])
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    print(f"Wrote {len(rows)} row(s) to {output_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
