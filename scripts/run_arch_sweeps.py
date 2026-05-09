#!/usr/bin/env python3
"""Run a matrix sweep across architecture JSONs and config knob variants."""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml


@dataclass(frozen=True)
class Variant:
    name: str
    description: str
    base_config: str | None
    overrides: dict[str, Any]


@dataclass(frozen=True)
class Experiment:
    name: str
    arch: str
    variant: Variant
    config_rel: str
    runmode: str
    jobs: int
    timeout_sec: int
    filters: tuple[str, ...]
    max_benches: int


@dataclass(frozen=True)
class ExperimentResult:
    experiment: Experiment
    return_code: int
    results_csv: Path
    temp_config: Path
    total: int
    ok: int
    failed: int
    sim_mismatch: int
    sim_missing: int


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    repo_root = default_repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=repo_root / "scripts" / "arch_sweeps.yaml",
    )
    parser.add_argument("--suite", default="edge_mem_arch_sweep")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory relative to --repo-root unless absolute.",
    )
    parser.add_argument(
        "--experiment-filter",
        action="append",
        default=[],
        help="Regex filter on expanded experiment names (repeatable).",
    )
    parser.add_argument(
        "--benchmark-filter",
        action="append",
        default=[],
        help="Regex filter passed through to run_all_benchmarks.py (repeatable).",
    )
    parser.add_argument(
        "--max-benches",
        type=int,
        help="Override max benchmark count after filtering for every experiment.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        help="Override jobs for every expanded experiment.",
    )
    parser.add_argument(
        "--experiment-jobs",
        type=int,
        default=1,
        help="Number of experiments to run in parallel.",
    )
    parser.add_argument(
        "--timeout-sec",
        type=int,
        help="Override timeout for every expanded experiment.",
    )
    parser.add_argument(
        "--runmode",
        choices=[
            "runall",
            "dfg_gen_only",
            "mapper_only",
            "sim_only",
            "artifact_sim_only",
            "runall_artifact_sim",
        ],
        help="Override runmode for every expanded experiment.",
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def load_manifest_suite(manifest_path: Path, suite_name: str) -> dict[str, Any]:
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


def sanitize_name(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("_")


def deep_merge(base: Any, override: Any) -> Any:
    if isinstance(base, dict) and isinstance(override, dict):
        merged = dict(base)
        for key, value in override.items():
            if key in merged:
                merged[key] = deep_merge(merged[key], value)
            else:
                merged[key] = value
        return merged
    return override


def read_yaml_mapping(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        data = yaml.safe_load(stream)
    if not isinstance(data, dict):
        raise ValueError(f"Expected top-level mapping in {path}")
    return data


def parse_variants(raw_variants: Any) -> list[Variant]:
    if not isinstance(raw_variants, list) or not raw_variants:
        raise ValueError("Suite must define a non-empty variants list.")

    variants: list[Variant] = []
    seen: set[str] = set()
    for raw in raw_variants:
        if not isinstance(raw, dict):
            raise ValueError("Each variant must be a mapping.")
        name = raw.get("name")
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Each variant must define non-empty string 'name'.")
        name = name.strip()
        if name in seen:
            raise ValueError(f"Duplicate variant name: {name}")
        seen.add(name)
        description = raw.get("description", "")
        if description is None:
            description = ""
        if not isinstance(description, str):
            raise ValueError(f"Variant {name}: description must be a string.")
        base_config = raw.get("base_config")
        if base_config is not None and (not isinstance(base_config, str) or not base_config.strip()):
            raise ValueError(f"Variant {name}: base_config must be a non-empty string when provided.")
        overrides = raw.get("overrides", {})
        if not isinstance(overrides, dict):
            raise ValueError(f"Variant {name}: overrides must be a mapping.")
        variants.append(
            Variant(
                name=name,
                description=description.strip(),
                base_config=(base_config.strip() if isinstance(base_config, str) else None),
                overrides=overrides,
            )
        )
    return variants


def parse_arches(raw_arches: Any) -> list[str]:
    if not isinstance(raw_arches, list) or not raw_arches:
        raise ValueError("Suite must define a non-empty arches list.")
    arches: list[str] = []
    seen: set[str] = set()
    for raw in raw_arches:
        if not isinstance(raw, str) or not raw.strip():
            raise ValueError("Each architecture entry must be a non-empty string.")
        arch = raw.strip()
        if arch in seen:
            raise ValueError(f"Duplicate architecture in sweep: {arch}")
        seen.add(arch)
        arches.append(arch)
    return arches


def expand_experiments(args: argparse.Namespace, suite: dict[str, Any]) -> list[Experiment]:
    base_config = suite.get("base_config", "config/default.yaml")
    if not isinstance(base_config, str) or not base_config.strip():
        raise ValueError("Suite base_config must be a non-empty string.")

    runmode = args.runmode or suite.get("runmode", "mapper_only")
    jobs = args.jobs if args.jobs is not None else int(suite.get("jobs", 1))
    timeout_sec = args.timeout_sec if args.timeout_sec is not None else int(suite.get("timeout_sec", 0))
    max_benches = (
        args.max_benches
        if args.max_benches is not None
        else int(suite.get("max_benches", 0))
    )
    raw_filters = suite.get("filters", [])
    if raw_filters is None:
        raw_filters = []
    if not isinstance(raw_filters, list) or not all(isinstance(item, str) for item in raw_filters):
        raise ValueError("Suite filters must be a list of regex strings.")
    filters = tuple(args.benchmark_filter) if args.benchmark_filter else tuple(raw_filters)

    common_overrides = suite.get("common_overrides", {})
    if not isinstance(common_overrides, dict):
        raise ValueError("Suite common_overrides must be a mapping.")

    arches = parse_arches(suite.get("arches"))
    variants = parse_variants(suite.get("variants"))

    experiments: list[Experiment] = []
    compiled_filters = [re.compile(pattern) for pattern in args.experiment_filter]
    for arch in arches:
        arch_stem = sanitize_name(Path(arch).stem)
        for variant in variants:
            exp_name = f"{variant.name}__{arch_stem}"
            if compiled_filters and not any(regex.search(exp_name) for regex in compiled_filters):
                continue
            variant_config = variant.base_config or base_config
            experiments.append(
                Experiment(
                    name=exp_name,
                    arch=arch,
                    variant=Variant(
                        name=variant.name,
                        description=variant.description,
                        base_config=variant_config,
                        overrides=deep_merge(common_overrides, variant.overrides),
                    ),
                    config_rel=variant_config,
                    runmode=runmode,
                    jobs=jobs,
                    timeout_sec=timeout_sec,
                    filters=filters,
                    max_benches=max_benches,
                )
            )
    if not experiments:
        raise ValueError("No experiments left after applying filters.")
    return experiments


def display_path(path: Path, repo_root: Path) -> str:
    try:
        return str(path.relative_to(repo_root))
    except ValueError:
        return str(path)


def write_temp_config(repo_root: Path, output_dir: Path, experiment: Experiment) -> Path:
    source_config = repo_root / experiment.config_rel
    config_data = read_yaml_mapping(source_config)
    merged = deep_merge(config_data, experiment.variant.overrides)
    merged["arch_template_json"] = experiment.arch

    temp_dir = output_dir / "configs"
    temp_dir.mkdir(parents=True, exist_ok=True)
    temp_path = temp_dir / f"{experiment.name}.yaml"
    with temp_path.open("w", encoding="utf-8") as stream:
        yaml.safe_dump(merged, stream, sort_keys=False)
    return temp_path


def count_results(csv_path: Path) -> tuple[int, int, int, int, int]:
    if not csv_path.exists():
        return 0, 0, 0, 0, 0
    total = 0
    ok = 0
    failed = 0
    sim_mismatch = 0
    sim_missing = 0
    with csv_path.open("r", encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            total += 1
            status = row.get("status")
            mismatches_raw = row.get("mismatches", "")
            mismatches = None if mismatches_raw == "" else int(mismatches_raw)
            if status == "ok":
                ok += 1
            else:
                failed += 1
            if mismatches is None:
                sim_missing += 1
            elif mismatches > 0:
                sim_mismatch += 1
    return total, ok, failed, sim_mismatch, sim_missing


def read_benchmark_rows(csv_path: Path) -> list[dict[str, str]]:
    if not csv_path.exists():
        return []
    with csv_path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def build_run_all_command(
    repo_root: Path,
    experiment: Experiment,
    temp_config: Path,
    output_dir: Path,
) -> tuple[list[str], Path]:
    exp_dir = output_dir / experiment.name
    results_csv = exp_dir / "results.csv"
    temp_config_rel = display_path(temp_config, repo_root)

    cmd = [
        sys.executable,
        str(repo_root / "scripts" / "run_all_benchmarks.py"),
        "--repo-root",
        str(repo_root),
        "--config",
        temp_config_rel,
        "--runmode",
        experiment.runmode,
        "--jobs",
        str(experiment.jobs),
        "--timeout-sec",
        str(experiment.timeout_sec),
        "--run-tag-prefix",
        experiment.name,
        "--output-root",
        "generated",
        "--output-csv",
        display_path(results_csv, repo_root),
    ]
    for pattern in experiment.filters:
        cmd.extend(["--filter", pattern])
    if experiment.max_benches > 0:
        cmd.extend(["--max-benches", str(experiment.max_benches)])
    return cmd, results_csv


def write_summary_csv(results: list[ExperimentResult], summary_csv: Path) -> None:
    summary_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "experiment",
        "arch",
        "variant",
        "runmode",
        "jobs",
        "timeout_sec",
        "return_code",
        "total",
        "ok",
        "failed",
        "sim_mismatch",
        "sim_missing",
        "results_csv",
        "config",
    ]
    with summary_csv.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for result in results:
            writer.writerow(
                {
                    "experiment": result.experiment.name,
                    "arch": result.experiment.arch,
                    "variant": result.experiment.variant.name,
                    "runmode": result.experiment.runmode,
                    "jobs": result.experiment.jobs,
                    "timeout_sec": result.experiment.timeout_sec,
                    "return_code": result.return_code,
                    "total": result.total,
                    "ok": result.ok,
                    "failed": result.failed,
                    "sim_mismatch": result.sim_mismatch,
                    "sim_missing": result.sim_missing,
                    "results_csv": str(result.results_csv),
                    "config": str(result.temp_config),
                }
            )


def write_combined_results_csv(results: list[ExperimentResult], combined_csv: Path) -> None:
    combined_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "experiment",
        "arch",
        "variant",
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
        "failure_stage",
        "failure_reason",
        "mapping_file",
        "result_file",
        "log_file",
        "run_tag",
    ]
    with combined_csv.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for result in results:
            for row in read_benchmark_rows(result.results_csv):
                writer.writerow(
                    {
                        "experiment": result.experiment.name,
                        "arch": result.experiment.arch,
                        "variant": result.experiment.variant.name,
                        "benchmark": row.get("benchmark", ""),
                        "function": row.get("function", ""),
                        "status": row.get("status", ""),
                        "return_code": row.get("return_code", ""),
                        "achieved_ii": row.get("achieved_ii", ""),
                        "ii_source": row.get("ii_source", ""),
                        "matches": row.get("matches", ""),
                        "mismatches": row.get("mismatches", ""),
                        "timeout": row.get("timeout", ""),
                        "elapsed_sec": row.get("elapsed_sec", ""),
                        "failure_stage": row.get("failure_stage", ""),
                        "failure_reason": row.get("failure_reason", ""),
                        "mapping_file": row.get("mapping_file", ""),
                        "result_file": row.get("result_file", ""),
                        "log_file": row.get("log_file", ""),
                        "run_tag": row.get("run_tag", ""),
                    }
                )


def run_one_experiment(
    index: int,
    total_count: int,
    repo_root: Path,
    resolved_output_dir: Path,
    experiment: Experiment,
) -> ExperimentResult:
    temp_config = write_temp_config(repo_root, resolved_output_dir, experiment)
    cmd, results_csv = build_run_all_command(
        repo_root, experiment, temp_config, resolved_output_dir
    )

    print(
        f"\n[{index}/{total_count}] {experiment.name}\n"
        f"  arch: {experiment.arch}\n"
        f"  variant: {experiment.variant.name}\n"
        f"  config: {display_path(temp_config, repo_root)}\n"
        f"  cmd: {' '.join(cmd)}"
    )
    sys.stdout.flush()

    completed = subprocess.run(cmd, cwd=repo_root, check=False)
    total, ok, failed, sim_mismatch, sim_missing = count_results(results_csv)
    return ExperimentResult(
        experiment=experiment,
        return_code=completed.returncode,
        results_csv=results_csv,
        temp_config=temp_config,
        total=total,
        ok=ok,
        failed=failed,
        sim_mismatch=sim_mismatch,
        sim_missing=sim_missing,
    )


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest if args.manifest.is_absolute() else repo_root / args.manifest
    suite = load_manifest_suite(manifest_path, args.suite)
    experiments = expand_experiments(args, suite)
    if args.experiment_jobs <= 0:
        raise ValueError("--experiment-jobs must be >= 1")

    output_dir = args.output_dir or Path("generated") / "morpher_runs" / "arch_sweeps" / args.suite
    resolved_output_dir = output_dir if output_dir.is_absolute() else repo_root / output_dir
    resolved_output_dir.mkdir(parents=True, exist_ok=True)

    if args.dry_run:
        print(f"Sweep suite: {args.suite}")
        print(f"Experiments: {len(experiments)}")
        for experiment in experiments:
            print(
                f"  {experiment.name}: arch={experiment.arch} "
                f"variant={experiment.variant.name} config={experiment.config_rel} "
                f"runmode={experiment.runmode}"
            )
        return 0

    configs_dir = resolved_output_dir / "configs"
    shutil.rmtree(configs_dir, ignore_errors=True)
    summary_csv = resolved_output_dir / "summary.csv"
    combined_csv = resolved_output_dir / "combined_results.csv"
    try:
        summary_csv.unlink()
    except FileNotFoundError:
        pass
    try:
        combined_csv.unlink()
    except FileNotFoundError:
        pass

    results: list[ExperimentResult] = []
    overall_rc = 0
    print(f"Sweep suite: {args.suite}")
    print(f"Expanded experiments: {len(experiments)}")
    print(f"Parallel experiments: {args.experiment_jobs}")
    sys.stdout.flush()

    indexed_results: list[tuple[int, ExperimentResult]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.experiment_jobs) as pool:
        futures = {
            pool.submit(
                run_one_experiment,
                index,
                len(experiments),
                repo_root,
                resolved_output_dir,
                experiment,
            ): index
            for index, experiment in enumerate(experiments, start=1)
        }
        for future in concurrent.futures.as_completed(futures):
            index = futures[future]
            result = future.result()
            print(
                f"  result[{index}/{len(experiments)}] {result.experiment.name}: "
                f"rc={result.return_code} total={result.total} ok={result.ok} failed={result.failed} "
                f"sim_mismatch={result.sim_mismatch} sim_missing={result.sim_missing}"
            )
            sys.stdout.flush()
            indexed_results.append((index, result))
            if result.return_code != 0:
                overall_rc = result.return_code

    indexed_results.sort(key=lambda item: item[0])
    results = [result for _, result in indexed_results]

    write_summary_csv(results, summary_csv)
    write_combined_results_csv(results, combined_csv)
    print(f"\nSummary CSV: {summary_csv}")
    print(f"Combined CSV: {combined_csv}")
    return overall_rc


if __name__ == "__main__":
    raise SystemExit(main())
