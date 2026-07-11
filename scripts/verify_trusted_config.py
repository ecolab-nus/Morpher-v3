#!/usr/bin/env python3
"""Fail if a config is not a trusted scalar/PF-only configuration."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "configs",
        nargs="*",
        default=["config/scalar.yaml"],
        help="Config YAML files to validate.",
    )
    return parser.parse_args()


def validate(path: Path) -> list[str]:
    from morpher_runner.context import RunnerConfig

    cfg = RunnerConfig.from_yaml(str(path))
    failures: list[str] = []

    expected_false = {
        "layout_solver": cfg.layout_solver,
        "replicate_data": cfg.replicate_data,
        "auto_unroll_enabled": cfg.auto_unroll_enabled,
        "arch_simd_enabled": cfg.arch_simd_enabled,
        "proximity_sort": cfg.proximity_sort,
    }
    for key, value in expected_false.items():
        if value:
            failures.append(f"{key} must be false")

    expected_equal = {
        "mapper_preset": (cfg.mapper_preset, "trusted_pf"),
        "mapping_method": (cfg.mapping_method, 0),
        "ii_restarts": (cfg.ii_restarts, 0),
        "off_spm_mem_candidate_topk": (cfg.off_spm_mem_candidate_topk, 0),
        "pf_candidate_topk": (cfg.pf_candidate_topk, 0),
        "pf_frontier_stall_repeats": (cfg.pf_frontier_stall_repeats, 8),
        "pf_frontier_stall_min_mapped": (cfg.pf_frontier_stall_min_mapped, 3),
        "rut_recovery_seed": (cfg.rut_recovery_seed, 0),
        "sa_stall_min_unmapped": (cfg.sa_stall_min_unmapped, 1),
        "source_template_mode": (cfg.source_template_mode, "off"),
    }
    for key, (actual, expected) in expected_equal.items():
        if actual != expected:
            failures.append(f"{key} must be {expected!r}, got {actual!r}")

    if sorted(cfg.auto_unroll_factors) != [1]:
        failures.append(f"auto_unroll_factors must be [1], got {cfg.auto_unroll_factors!r}")

    return failures


def display_path(path: Path, root: Path):
    try:
        return path.relative_to(root)
    except ValueError:
        return str(path)


def main() -> int:
    root = repo_root()
    os.chdir(root)
    sys.path.insert(0, str(root))
    args = parse_args()

    failed = False
    for config in args.configs:
        path = Path(config)
        if not path.is_absolute():
            path = root / path
        failures = validate(path)
        display = display_path(path, root)
        if failures:
            failed = True
            print(f"{display}: not trusted")
            for failure in failures:
                print(f"  - {failure}")
        else:
            print(f"{display}: trusted scalar config OK")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
