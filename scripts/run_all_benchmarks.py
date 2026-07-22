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
import datetime
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

import yaml

_REPO_IMPORT_ROOT = str(Path(__file__).resolve().parents[1])
if _REPO_IMPORT_ROOT not in sys.path:
    sys.path.insert(0, _REPO_IMPORT_ROOT)

from morpher_runner.memory_profile import CSV_FIELDS as MEMORY_PROFILE_CSV_FIELDS
from morpher_runner.memory_profile import profile_csv_fields


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
    kernel_run_dir: Path
    run_tag: str
    failure_stage: str
    failure_reason: str
    achieved_ii: int | None
    ii_source: str | None
    selected_unroll_factor: int | None
    auto_unroll_score: float | None
    auto_unroll_results_file: Path | None
    mapping_file: Path | None
    result_file: Path | None
    matches: int | None
    mismatches: int | None
    memory_profile_fields: dict[str, object]
    recovered_from_timeout: bool = False
    recovery_reason: str = ""

    @property
    def status(self) -> str:
        if self.recovered_from_timeout:
            return "ok"
        if self.timeout:
            return "timeout"
        if self.return_code == 0:
            return "ok"
        return "failed"

    @property
    def best_score(self) -> float | int | None:
        if self.auto_unroll_score is not None:
            return self.auto_unroll_score
        return self.achieved_ii


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


def read_config_summary(repo_root: Path, config_rel: str) -> tuple[str, str]:
    config_path = repo_root / config_rel
    with config_path.open("r", encoding="utf-8") as stream:
        config_dict = yaml.safe_load(stream)
    if not isinstance(config_dict, dict):
        raise ValueError(f"Config {config_path} must contain a top-level mapping.")

    arch = str(config_dict.get("arch_template_json", "<unset>"))
    auto_unroll = config_dict.get("auto_unroll", {})
    if isinstance(auto_unroll, dict):
        unroll_enabled = bool(auto_unroll.get("enabled", False))
        unroll_factors = auto_unroll.get("factors", [])
    else:
        unroll_enabled = bool(auto_unroll)
        unroll_factors = []
    factors = ",".join(str(item) for item in unroll_factors) if unroll_factors else "-"
    pipeline = config_dict.get("dfg_pipeline", {}) or {}
    externalize = pipeline.get("externalize_access", "off") if isinstance(pipeline, dict) else "off"
    simd_pack = pipeline.get("simd_pack", "off") if isinstance(pipeline, dict) else "off"
    knobs = [
        f"externalize={externalize}",
        f"simd_pack={simd_pack}",
        f"unroll={'on' if unroll_enabled else 'off'}[{factors}]",
        f"layout={int(bool(config_dict.get('layout_solver', False)))}",
        f"repl={int(bool(config_dict.get('replicate_data', False)))}",
        f"preset={config_dict.get('mapper_preset', '<unset>')}",
    ]
    return arch, ",".join(knobs)


def benchmark_label(rel_source: str, function: str) -> str:
    path = Path(rel_source)
    kernel = path.parent.name or path.stem
    if function and function != kernel:
        return f"{kernel}/{function}"
    return kernel


_SIM_RESULT_RE = re.compile(
    r"Simulation Result:\s*Matches::\s*(\d+)\s*,\s*Mismatches::\s*(\d+)"
)
_MAP_SUCCESS_II_RE = re.compile(r"Map Success with II =\s*(\d+)")
_II_IN_FILENAME_RE = re.compile(r"_II=(\d+)")
_ERROR_LINE_RE = re.compile(
    r"(Error:|error:|RuntimeError:|FileNotFoundError:|CalledProcessError|"
    r"Assertion|Traceback|Segmentation fault|Aborted|\[TIMEOUT\])"
)


_ACTIVE_PROCESSES: set[subprocess.Popen] = set()
_ACTIVE_PROCESSES_LOCK = threading.Lock()
_SHUTDOWN_REQUESTED = threading.Event()


def child_pids(pid: int) -> list[int]:
    try:
        output = subprocess.check_output(["pgrep", "-P", str(pid)], text=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    return [int(line) for line in output.splitlines() if line.strip().isdigit()]


def descendant_pids(pid: int) -> list[int]:
    out: list[int] = []
    stack = child_pids(pid)
    while stack:
        child = stack.pop()
        out.append(child)
        stack.extend(child_pids(child))
    return out


def signal_pid_or_group(pid: int, sig: int) -> None:
    try:
        pgid = os.getpgid(pid)
    except ProcessLookupError:
        return
    try:
        os.killpg(pgid, sig)
    except ProcessLookupError:
        pass
    try:
        os.kill(pid, sig)
    except ProcessLookupError:
        pass


def terminate_process_group(process: subprocess.Popen, grace_sec: float = 3.0) -> None:
    descendants = descendant_pids(process.pid)
    for pid in sorted(descendants, reverse=True):
        signal_pid_or_group(pid, signal.SIGTERM)

    signal_pid_or_group(process.pid, signal.SIGTERM)

    try:
        process.wait(timeout=grace_sec)
        return
    except subprocess.TimeoutExpired:
        pass

    descendants = descendant_pids(process.pid)
    for pid in sorted(descendants, reverse=True):
        signal_pid_or_group(pid, signal.SIGKILL)
    signal_pid_or_group(process.pid, signal.SIGKILL)
    process.wait()


def register_process(process: subprocess.Popen) -> None:
    with _ACTIVE_PROCESSES_LOCK:
        _ACTIVE_PROCESSES.add(process)


def unregister_process(process: subprocess.Popen) -> None:
    with _ACTIVE_PROCESSES_LOCK:
        _ACTIVE_PROCESSES.discard(process)


def terminate_active_process_groups() -> None:
    with _ACTIVE_PROCESSES_LOCK:
        processes = list(_ACTIVE_PROCESSES)
    for process in processes:
        terminate_process_group(process)


def install_signal_cleanup() -> None:
    def handle_signal(signum: int, _frame: object) -> None:
        _SHUTDOWN_REQUESTED.set()
        terminate_active_process_groups()
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)


def run_logged_process(
    cmd: list[str],
    *,
    cwd: Path,
    log_file: Path,
    timeout_sec: int,
    env: dict[str, str],
) -> subprocess.CompletedProcess:
    if _SHUTDOWN_REQUESTED.is_set():
        raise SystemExit(130)
    timeout = None if timeout_sec <= 0 else timeout_sec
    with log_file.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            cmd,
            cwd=cwd,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
            start_new_session=True,
        )
        register_process(process)
        if _SHUTDOWN_REQUESTED.is_set():
            terminate_process_group(process)
            raise SystemExit(130)
        try:
            process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            terminate_process_group(process)
            raise
        except BaseException:
            terminate_process_group(process)
            raise
        finally:
            unregister_process(process)
    return subprocess.CompletedProcess(cmd, process.returncode)


STAGE_MARKERS: list[tuple[str, str]] = [
    ("artifact_simulator", "-----Running Morpher artifact_simulator ("),
    ("simulator", "[sim "),
    ("mapper", "[2/2] Run mapper"),
    ("mapper", "[1/1] Run mapper"),
    ("mapper_mem_alloc", "[1/2] Update memory allocation"),
    ("mapper_mem_alloc", "Updating memory allocation.."),
    ("mapper_setup", "-----Running Morpher_CGRA_Mapper-----"),
    ("trace_gen", "Running instrumented code to generate"),
    ("instrumentation", "Code instrumentation.."),
    ("simd_pack", "SIMD pack:"),
    ("dfg_dot_render", "_PartPredDFG.pdf"),
    ("dfg_gen", "Generating DFG ("),
    ("unroll", "Unrolling annotated loop for DFG candidate.."),
    ("ir_opt", "Optimizing IR.."),
    ("ir_gen", "Generating IR.."),
    ("dfg_setup", "-----Running Morpher_DFG_Generator-----"),
]


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


def infer_last_stage(log_text: str) -> str:
    last_stage = "setup"
    for line in log_text.splitlines():
        for stage, marker in STAGE_MARKERS:
            if marker in line:
                last_stage = stage
    return last_stage


def infer_failure_stage(log_text: str, timed_out: bool, return_code: int | None) -> str:
    if not timed_out and return_code == 0:
        return ""

    if "DFG generation step finished without required output files" in log_text:
        return "dfg_gen"
    if "DFG generation does not support this kernel's array dimensionality" in log_text:
        return "dfg_gen"
    if "DFG generation failed during SPM bank allocation" in log_text:
        return "dfg_gen"
    if "DFG XML missing <DFG> element" in log_text:
        return "simd_pack"
    if "_PartPredDFG.dot: syntax error" in log_text:
        return "dfg_dot_render"
    if "morpher-loop-unroll" in log_text and "returned non-zero exit" in log_text:
        return "unroll"
    if "opt:" in log_text and "_uf" in log_text and "_opt.ll: error: Could not open input file" in log_text:
        return "dfg_gen"
    if "No CGRA config binaries found" in log_text:
        return "simulator_setup"
    if "No trace files found" in log_text:
        return "simulator_setup"
    return infer_last_stage(log_text)


def infer_failure_reason(log_text: str, timed_out: bool, return_code: int | None) -> str:
    if not timed_out and return_code == 0:
        return ""
    if timed_out:
        return "command exceeded timeout"

    preferred_messages = [
        "Error: DFG generation does not support this kernel's array dimensionality.",
        "Error: DFG generation failed during SPM bank allocation.",
        "DFG generation step finished without required output files",
        "DFG XML missing <DFG> element",
        "_PartPredDFG.dot: syntax error",
        "No CGRA config binaries found",
        "No trace files found",
    ]
    for message in preferred_messages:
        if message in log_text:
            if "\n" not in message:
                for line in log_text.splitlines():
                    if message in line:
                        return line.strip()[:300]
            return message[:300]

    interesting = []
    for line in log_text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if _ERROR_LINE_RE.search(stripped):
            interesting.append(stripped)

    if interesting:
        return interesting[-1][:300]

    lines = [line.strip() for line in log_text.splitlines() if line.strip()]
    return lines[-1][:300] if lines else "no log output"


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


def parse_auto_unroll_selection(auto_results_file: Path, start_time: float) -> tuple[int | None, float | None, int | None]:
    if not auto_results_file.exists():
        return None, None, None
    try:
        if auto_results_file.stat().st_mtime < start_time:
            return None, None, None
        with auto_results_file.open("r", encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream))
    except (OSError, ValueError):
        return None, None, None

    selected = None
    for row in rows:
        if str(row.get("selected", "")).strip() == "1":
            selected = row
            break
    if selected is None:
        for row in rows:
            if str(row.get("promoted", "")).strip() == "1":
                selected = row
    if selected is None:
        return None, None, None

    try:
        factor = int(str(selected.get("factor", "")).strip())
    except ValueError:
        factor = None
    try:
        score = float(str(selected.get("score", "")).strip())
    except ValueError:
        score = None
    try:
        ii = int(str(selected.get("ii", "")).strip())
    except ValueError:
        ii = None
    return factor, score, ii


@dataclass(frozen=True)
class RunMetadata:
    log_text: str
    matches: int | None
    mismatches: int | None
    failure_stage: str
    failure_reason: str
    achieved_ii: int | None
    ii_source: str | None
    selected_unroll_factor: int | None
    auto_unroll_score: float | None
    auto_unroll_results_file: Path | None
    mapping_file: Path | None
    result_file: Path | None
    memory_profile_fields: dict[str, object]


def collect_run_metadata(
    *,
    log_file: Path,
    mapper_kernel_dir: Path,
    sim_kernel_dir: Path,
    start_time: float,
    timed_out: bool,
    return_code: int | None,
) -> RunMetadata:
    log_text = log_file.read_text(encoding="utf-8", errors="ignore")
    matches, mismatches = parse_matches_mismatches(log_text)
    failure_stage = infer_failure_stage(log_text, timed_out, return_code)
    failure_reason = infer_failure_reason(log_text, timed_out, return_code)
    mapper_bins = list(mapper_kernel_dir.glob("*.bin")) if mapper_kernel_dir.exists() else []
    sim_bins = list(sim_kernel_dir.glob("*.bin")) if sim_kernel_dir.exists() else []
    mapping_files = (
        list(mapper_kernel_dir.glob("*.mappingwithlatency.txt"))
        if mapper_kernel_dir.exists()
        else []
    )
    result_file = mapper_kernel_dir / "result.txt"
    auto_unroll_results_file = mapper_kernel_dir / "auto_unroll_results.csv"
    try:
        memory_profile_fields = profile_csv_fields(
            sim_kernel_dir / "memory_profile.json"
        )
    except (OSError, RuntimeError, ValueError):
        memory_profile_fields = {
            field: "" for field in MEMORY_PROFILE_CSV_FIELDS
        }

    achieved_ii = None
    ii_source = None
    mapping_file = None

    res_ii = ii_from_result_txt(result_file, start_time)
    if res_ii is not None:
        achieved_ii = res_ii
        ii_source = "result_txt"
        mapping_file = newest_file_for_ii(mapping_files, start_time, achieved_ii)
    else:
        map_ii, map_file = newest_ii_from_files(mapping_files, start_time)
        if map_ii is not None:
            achieved_ii = map_ii
            ii_source = "mapping_txt"
            mapping_file = map_file
        else:
            bin_ii = newest_ii_from_bins(mapper_bins + sim_bins, start_time)
            if bin_ii is not None:
                achieved_ii = bin_ii
                ii_source = "bin_file"
            else:
                log_ii = parse_ii_from_log(log_text)
                if log_ii is not None:
                    achieved_ii = log_ii
                    ii_source = "mapper_log"

    selected_unroll_factor, auto_unroll_score, auto_unroll_ii = parse_auto_unroll_selection(
        auto_unroll_results_file,
        start_time,
    )
    if achieved_ii is None and auto_unroll_ii is not None:
        achieved_ii = auto_unroll_ii
        ii_source = "auto_unroll_csv"

    return RunMetadata(
        log_text=log_text,
        matches=matches,
        mismatches=mismatches,
        failure_stage=failure_stage,
        failure_reason=failure_reason,
        achieved_ii=achieved_ii,
        ii_source=ii_source,
        selected_unroll_factor=selected_unroll_factor,
        auto_unroll_score=auto_unroll_score,
        auto_unroll_results_file=(auto_unroll_results_file if auto_unroll_results_file.exists() else None),
        mapping_file=mapping_file,
        result_file=(result_file if result_file.exists() else None),
        memory_profile_fields=memory_profile_fields,
    )


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
        default="config/default.yaml",
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
        "--output-root",
        type=Path,
        default=Path("generated"),
        help="Root output directory. Each kernel writes to <root>/<suite>/<kernel>/<timestamp>/. Relative to --repo-root.",
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
    parser.add_argument(
        "--run-tag-prefix",
        default="",
        help="Optional prefix added to each benchmark run_tag to avoid workspace collisions across concurrent batches.",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="Keep existing ok/failed rows in --output-csv and rerun only timeout or missing benchmark rows.",
    )
    parser.add_argument(
        "--resume-missing",
        action="store_true",
        help="Resume CSV state and rerun only benchmarks with no existing row or an empty status.",
    )
    parser.add_argument(
        "--resume-timeout",
        action="store_true",
        help="Resume CSV state and rerun only existing timeout rows.",
    )
    return parser.parse_args()


def sanitize_name(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "__", text)


def workspace_run_dirs(repo_root: Path, run_tag: str, appfolder: str, mapper_subfolder: str) -> tuple[Path, Path, Path]:
    run_root = repo_root / "workspace" / "morpher_runs" / run_tag
    return (
        run_root / "dfg" / appfolder,
        run_root / "mapper" / mapper_subfolder / appfolder,
        run_root / "simulator" / appfolder,
    )


_ARCH_VEC_WIDTH_RE = re.compile(r"(?:^|[_/-])vec(\d+)(?:[_./-]|$)")


def fallback_unroll_factor_for_config(config_dict: dict) -> int:
    arch_template = str(config_dict.get("arch_template_json", ""))
    match = _ARCH_VEC_WIDTH_RE.search(arch_template)
    if match:
        width = int(match.group(1))
        if width in (4, 8):
            return width
    return 1


def read_fallback_unroll_factor(repo_root: Path, config_rel: str) -> int:
    source_config = repo_root / config_rel
    with source_config.open("r", encoding="utf-8") as stream:
        config_dict = yaml.safe_load(stream)
    if not isinstance(config_dict, dict):
        raise ValueError(f"Config {source_config} must contain a top-level mapping.")
    return fallback_unroll_factor_for_config(config_dict)


def write_fallback_unroll_config(
    repo_root: Path,
    config_rel: str,
    output_path: Path,
    fallback_factor: int | None = None,
) -> int:
    source_config = repo_root / config_rel
    with source_config.open("r", encoding="utf-8") as stream:
        config_dict = yaml.safe_load(stream)
    if not isinstance(config_dict, dict):
        raise ValueError(f"Config {source_config} must contain a top-level mapping.")

    if fallback_factor is None:
        fallback_factor = fallback_unroll_factor_for_config(config_dict)
    auto_unroll = config_dict.get("auto_unroll", {})
    if not isinstance(auto_unroll, dict):
        auto_unroll = {"enabled": bool(auto_unroll)}
    else:
        auto_unroll = dict(auto_unroll)
    auto_unroll["enabled"] = True
    auto_unroll["factors"] = [fallback_factor]
    auto_unroll["stop_on_degradation"] = True
    config_dict["auto_unroll"] = auto_unroll

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as stream:
        yaml.safe_dump(config_dict, stream, sort_keys=False)
    return fallback_factor


def discover_all_benchmarks(repo_root: Path) -> list[BenchmarkSpec]:
    bench_root = repo_root / BMARK_ROOT
    if not bench_root.exists():
        raise FileNotFoundError(f"Benchmark root not found: {bench_root}")

    all_c_files = sorted(
        path
        for path in bench_root.rglob("*.c")
        if "__run_" not in path.relative_to(bench_root).as_posix()
    )
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
        print(
            "WARNING: skipping benchmark .c files outside required layout "
            "'<suite>/<kernel>/<kernel>.c':\n"
            f"  - {bad}{extra}",
            flush=True,
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
    output_root: Path,
    mapper_subfolder: str,
    run_tag_prefix: str,
) -> RunResult:
    safe = sanitize_name(spec.rel_source)
    source_stem = safe[:-2] if safe.endswith(".c") else safe
    run_tag = f"{idx:03d}_{source_stem}"
    if run_tag_prefix:
        run_tag = f"{sanitize_name(run_tag_prefix)}__{run_tag}"

    rel_parts = Path(spec.rel_source).parts  # (<suite>, <kernel>, <kernel>.c)
    suite, kernel = rel_parts[0], rel_parts[1]
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    kernel_run_dir = output_root / suite / kernel / timestamp
    kernel_run_dir.mkdir(parents=True, exist_ok=True)
    log_file = kernel_run_dir / "log.txt"

    cmd = [
        sys.executable,
        "-u",
        "run_morpher.py",
        spec.rel_source,
        spec.function,
        config_rel,
        runmode,
        "--run-tag",
        run_tag,
    ]
    start = time.time()
    rc = None
    timed_out = False

    appfolder = str(Path(spec.rel_source).parent).replace("\\", "/")
    dfg_kernel_dir, mapper_kernel_dir, sim_kernel_dir = workspace_run_dirs(
        repo_root, run_tag, appfolder, mapper_subfolder
    )
    for work_dir in (dfg_kernel_dir, mapper_kernel_dir, sim_kernel_dir):
        if work_dir.exists():
            shutil.rmtree(work_dir)

    env = os.environ.copy()

    try:
        completed = run_logged_process(
            cmd,
            cwd=repo_root,
            log_file=log_file,
            timeout_sec=timeout_sec,
            env=env,
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

    metadata = collect_run_metadata(
        log_file=log_file,
        mapper_kernel_dir=mapper_kernel_dir,
        sim_kernel_dir=sim_kernel_dir,
        start_time=start,
        timed_out=timed_out,
        return_code=rc,
    )
    result_log_file = log_file
    result_run_tag = run_tag
    recovered_from_timeout = False
    recovery_reason = ""

    if (
        timed_out
        and metadata.selected_unroll_factor is not None
        and metadata.achieved_ii is not None
    ):
        recovered_from_timeout = True
        recovery_reason = "partial_auto_unroll_result"

    if timed_out and not recovered_from_timeout and runmode == "runall":
        fallback_factor = read_fallback_unroll_factor(repo_root, config_rel)
        fallback_run_tag = f"{run_tag}__fallback_uf{fallback_factor}"
        fallback_dfg_dir, fallback_mapper_dir, fallback_sim_dir = workspace_run_dirs(
            repo_root, fallback_run_tag, appfolder, mapper_subfolder
        )
        for work_dir in (fallback_dfg_dir, fallback_mapper_dir, fallback_sim_dir):
            if work_dir.exists():
                shutil.rmtree(work_dir)

        fallback_config = kernel_run_dir / f"fallback_uf{fallback_factor}_config.yaml"
        fallback_log = kernel_run_dir / f"fallback_uf{fallback_factor}_log.txt"
        write_fallback_unroll_config(
            repo_root,
            config_rel,
            fallback_config,
            fallback_factor=fallback_factor,
        )
        fallback_config_rel = os.path.relpath(fallback_config, repo_root)
        fallback_cmd = [
            sys.executable,
            "-u",
            "run_morpher.py",
            spec.rel_source,
            spec.function,
            fallback_config_rel,
            runmode,
            "--run-tag",
            fallback_run_tag,
        ]
        fallback_start = time.time()
        fallback_rc = None
        fallback_timed_out = False
        with log_file.open("a", encoding="utf-8") as log:
            log.write(
                "\n[FALLBACK] no partial auto-unroll result; "
                f"trying unroll={fallback_factor} with 900s timeout\n"
            )
            log.write("[FALLBACK] log: %s\n" % fallback_log)
        try:
            completed = run_logged_process(
                fallback_cmd,
                cwd=repo_root,
                log_file=fallback_log,
                timeout_sec=900,
                env=env,
            )
            fallback_rc = completed.returncode
        except subprocess.TimeoutExpired as exc:
            fallback_timed_out = True
            with fallback_log.open("a", encoding="utf-8") as log:
                log.write(
                    f"\n[TIMEOUT] fallback unroll={fallback_factor} command exceeded 900s\n"
                )
                if exc.stdout:
                    log.write(str(exc.stdout))
                if exc.stderr:
                    log.write(str(exc.stderr))

        fallback_metadata = collect_run_metadata(
            log_file=fallback_log,
            mapper_kernel_dir=fallback_mapper_dir,
            sim_kernel_dir=fallback_sim_dir,
            start_time=fallback_start,
            timed_out=fallback_timed_out,
            return_code=fallback_rc,
        )
        if (
            not fallback_timed_out
            and fallback_rc == 0
            and fallback_metadata.achieved_ii is not None
        ):
            metadata = fallback_metadata
            rc = fallback_rc
            result_log_file = fallback_log
            result_run_tag = fallback_run_tag
            recovered_from_timeout = True
            recovery_reason = f"fallback_unroll{fallback_factor}"
        else:
            with log_file.open("a", encoding="utf-8") as log:
                log.write(
                    f"[FALLBACK] unroll={fallback_factor} did not produce a usable result "
                    f"(timeout={fallback_timed_out}, return_code={fallback_rc}, "
                    f"ii={fallback_metadata.achieved_ii})\n"
                )

    elapsed = round(time.time() - start, 2)
    result = RunResult(
        idx=idx,
        total=total,
        rel_source=spec.rel_source,
        function=spec.function,
        return_code=rc,
        timeout=timed_out,
        elapsed_sec=elapsed,
        log_file=result_log_file,
        kernel_run_dir=kernel_run_dir,
        run_tag=result_run_tag,
        failure_stage="" if recovered_from_timeout else metadata.failure_stage,
        failure_reason="" if recovered_from_timeout else metadata.failure_reason,
        achieved_ii=metadata.achieved_ii,
        ii_source=metadata.ii_source,
        selected_unroll_factor=metadata.selected_unroll_factor,
        auto_unroll_score=metadata.auto_unroll_score,
        auto_unroll_results_file=metadata.auto_unroll_results_file,
        mapping_file=metadata.mapping_file,
        result_file=metadata.result_file,
        matches=metadata.matches,
        mismatches=metadata.mismatches,
        memory_profile_fields=metadata.memory_profile_fields,
        recovered_from_timeout=recovered_from_timeout,
        recovery_reason=recovery_reason,
    )

    try:
        shutil.copy2(repo_root / config_rel, kernel_run_dir / "config.yaml")
    except OSError:
        pass
    write_csv([result], kernel_run_dir / "result.csv")

    return result


CSV_FIELDS = [
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
] + MEMORY_PROFILE_CSV_FIELDS


def run_result_to_row(r: RunResult) -> dict[str, object]:
    row = {
        "benchmark": r.rel_source,
        "function": r.function,
        "status": r.status,
        "return_code": r.return_code,
        "achieved_ii": r.achieved_ii,
        "ii_source": r.ii_source,
        "best_score": r.best_score,
        "selected_unroll_factor": r.selected_unroll_factor,
        "auto_unroll_score": r.auto_unroll_score,
        "matches": r.matches,
        "mismatches": r.mismatches,
        "timeout": r.timeout,
        "recovered_from_timeout": r.recovered_from_timeout,
        "recovery_reason": r.recovery_reason,
        "elapsed_sec": r.elapsed_sec,
        "run_dir": str(r.kernel_run_dir),
        "log_file": str(r.log_file),
        "run_tag": r.run_tag,
        "failure_stage": r.failure_stage,
        "failure_reason": r.failure_reason,
        "mapping_file": (str(r.mapping_file) if r.mapping_file else ""),
        "result_file": (str(r.result_file) if r.result_file else ""),
        "auto_unroll_results_file": (
            str(r.auto_unroll_results_file) if r.auto_unroll_results_file else ""
        ),
    }
    row.update(r.memory_profile_fields)
    return row


def write_csv(results: list[RunResult], out_csv: Path) -> None:
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for r in results:
            writer.writerow(run_result_to_row(r))


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", newline="", encoding="utf-8") as fp:
        return list(csv.DictReader(fp))


def write_rows(rows: list[dict[str, object]], out_csv: Path) -> None:
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in CSV_FIELDS})


def row_key(row: dict[str, str]) -> tuple[str, str]:
    return row.get("benchmark", ""), row.get("function", "")


def spec_key(spec: BenchmarkSpec) -> tuple[str, str]:
    return spec.rel_source, spec.function


def row_has_timeout_status(row: dict[str, str]) -> bool:
    status = str(row.get("status", "")).strip().lower()
    timeout = str(row.get("timeout", "")).strip().lower()
    return status == "timeout" or timeout == "true"


def row_has_transient_failure(row: dict[str, str]) -> bool:
    status = str(row.get("status", "")).strip().lower()
    stage = str(row.get("failure_stage", "")).strip().lower()
    reason = str(row.get("failure_reason", "")).strip().lower()
    return (
        status == "failed"
        and (
            stage == "setup"
            or not reason
            or reason == "no log output"
            or reason.startswith("/_/")
        )
    )


def row_should_rerun_for_resume(
    row: dict[str, str] | None,
    *,
    rerun_missing: bool,
    rerun_timeout: bool,
    rerun_transient_failures: bool,
) -> bool:
    if row is None:
        return rerun_missing
    status = str(row.get("status", "")).strip().lower()
    if not status:
        return rerun_missing
    if status == "ok":
        return False
    if rerun_transient_failures and row_has_transient_failure(row):
        return True
    return rerun_timeout and row_has_timeout_status(row)


def load_resume_rows(
    output_csv: Path,
    output_root: Path,
    selected: list[BenchmarkSpec],
    run_tag_prefix: str,
) -> list[dict[str, str]]:
    rows = read_csv_rows(output_csv)
    if rows:
        return rows

    recovered: dict[tuple[str, str], tuple[float, dict[str, str]]] = {}
    prefix = sanitize_name(run_tag_prefix)
    for spec in selected:
        rel_parts = Path(spec.rel_source).parts
        if len(rel_parts) < 2:
            continue
        suite, kernel = rel_parts[0], rel_parts[1]
        for result_csv in (output_root / suite / kernel).glob("*/result.csv"):
            try:
                mtime = result_csv.stat().st_mtime
                candidate_rows = read_csv_rows(result_csv)
            except OSError:
                continue
            for row in candidate_rows:
                if row_key(row) != spec_key(spec):
                    continue
                row_run_tag = str(row.get("run_tag", ""))
                if prefix and not row_run_tag.startswith(prefix + "__"):
                    continue
                key = spec_key(spec)
                prev = recovered.get(key)
                if prev is None or mtime > prev[0]:
                    recovered[key] = (mtime, row)

    return [row for _mtime, row in recovered.values()]


def main() -> int:
    install_signal_cleanup()
    args = parse_args()
    if args.jobs <= 0:
        raise ValueError("--jobs must be >= 1")

    repo_root = args.repo_root.resolve()
    if not (repo_root / "run_morpher.py").exists():
        raise FileNotFoundError(f"run_morpher.py not found under {repo_root}")
    mapper_subfolder = read_mapper_subfolder(repo_root, args.config)
    arch_name, config_knobs = read_config_summary(repo_root, args.config)

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

    output_root = (repo_root / args.output_root).resolve()
    output_csv = (repo_root / args.output_csv).resolve()
    original_selected = list(selected)
    preserved_rows: list[dict[str, str]] = []
    explicit_resume_policy = args.resume_missing or args.resume_timeout
    resume_enabled = args.resume or explicit_resume_policy
    rerun_missing = args.resume_missing
    rerun_timeout = args.resume_timeout
    rerun_transient_failures = False
    if args.resume and not explicit_resume_policy:
        rerun_missing = True
        rerun_timeout = True
        rerun_transient_failures = True

    if resume_enabled:
        existing_rows = load_resume_rows(
            output_csv,
            output_root,
            selected,
            args.run_tag_prefix,
        )
        existing_by_key = {row_key(row): row for row in existing_rows}
        selected_to_run = []
        for spec in selected:
            existing = existing_by_key.get(spec_key(spec))
            if row_should_rerun_for_resume(
                existing,
                rerun_missing=rerun_missing,
                rerun_timeout=rerun_timeout,
                rerun_transient_failures=rerun_transient_failures,
            ):
                selected_to_run.append(spec)
            elif existing is not None:
                preserved_rows.append(existing)
        ignored_missing = sum(
            1
            for spec in selected
            if existing_by_key.get(spec_key(spec)) is None
            and not row_should_rerun_for_resume(
                None,
                rerun_missing=rerun_missing,
                rerun_timeout=rerun_timeout,
                rerun_transient_failures=rerun_transient_failures,
            )
        )
        selected = selected_to_run
        policy = []
        if rerun_missing:
            policy.append("missing")
        if rerun_timeout:
            policy.append("timeout")
        if rerun_transient_failures:
            policy.append("transient-failure")
        policy_text = ",".join(policy) if policy else "none"
        print(
            f"Resume mode ({policy_text}): preserving {len(preserved_rows)} existing row(s), "
            f"rerunning {len(selected)} selected row(s), "
            f"leaving {ignored_missing} missing row(s) absent."
        )

    if resume_enabled and not selected:
        write_rows(preserved_rows, output_csv)
        failed = sum(1 for row in preserved_rows if str(row.get("status", "")).strip() != "ok")
        print("No benchmark rows matched the resume policy.")
        print(f"  csv: {output_csv}")
        return 0 if failed == 0 else 1

    total = len(selected)
    print(f"Launching {total} benchmarks with {args.jobs} parallel worker(s)...")
    print(f"Config: arch={arch_name} knobs={config_knobs}")

    indexed_results: list[tuple[int, RunResult]] = []
    current_rows_by_key: dict[tuple[str, str], dict[str, object]] = {
        row_key(row): row for row in preserved_rows
    }
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
                output_root=output_root,
                mapper_subfolder=mapper_subfolder,
                run_tag_prefix=args.run_tag_prefix,
            ): idx
            for idx, spec in enumerate(selected, start=1)
        }
        for future in concurrent.futures.as_completed(futures):
            idx = futures[future]
            result = future.result()
            failure = "-"
            if result.status != "ok":
                failure = result.failure_stage or "unknown"
                if result.failure_reason:
                    failure = f"{failure}:{result.failure_reason}"
            elif result.recovered_from_timeout:
                failure = f"recovered:{result.recovery_reason}"
            print(
                f"[{result.idx}/{result.total}] {benchmark_label(result.rel_source, result.function)} :: "
                f"status={result.status} fail={failure} ii={result.achieved_ii} "
                f"score={result.best_score} unroll={result.selected_unroll_factor} "
                f"mismatches={result.mismatches} elapsed={result.elapsed_sec}s "
                f"arch={arch_name} knobs={config_knobs}"
            )
            indexed_results.append((idx, result))
            current_rows_by_key[(result.rel_source, result.function)] = run_result_to_row(result)
            if resume_enabled:
                incremental_rows = []
                for spec in original_selected:
                    row = current_rows_by_key.get(spec_key(spec))
                    if row is not None:
                        incremental_rows.append(row)
                write_rows(incremental_rows, output_csv)
            else:
                partial_results = [
                    item_result
                    for _, item_result in sorted(indexed_results, key=lambda item: item[0])
                ]
                write_csv(partial_results, output_csv)

    indexed_results.sort(key=lambda item: item[0])
    results = [result for _, result in indexed_results]
    if resume_enabled:
        new_by_key = {
            (result.rel_source, result.function): run_result_to_row(result)
            for result in results
        }
        preserved_by_key = {row_key(row): row for row in preserved_rows}
        merged_rows = []
        for spec in original_selected:
            key = spec_key(spec)
            if key in new_by_key:
                merged_rows.append(new_by_key[key])
            elif key in preserved_by_key:
                merged_rows.append(preserved_by_key[key])
        write_rows(merged_rows, output_csv)
        failed = sum(1 for row in merged_rows if str(row.get("status", "")).strip() != "ok")
    else:
        failed = sum(1 for r in results if r.status != "ok")
        write_csv(results, output_csv)
    stage_counts = {}
    for result in results:
        if result.status == "ok":
            continue
        stage = result.failure_stage or "unknown"
        stage_counts[stage] = stage_counts.get(stage, 0) + 1

    print("\nSummary:")
    print(f"  total benchmarks: {len(original_selected) if resume_enabled else len(results)}")
    if resume_enabled:
        print(f"  rerun benchmarks: {len(results)}")
    print(f"  failed benchmarks: {failed}")
    if stage_counts:
        print("  failures by stage:")
        for stage, count in sorted(stage_counts.items()):
            print(f"    {stage}: {count}")
    print(f"  csv: {output_csv}")
    print(f"  output root: {output_root}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
