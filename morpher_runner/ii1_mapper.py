"""Orchestrator for II=1 partitioned CGRA mapping."""
import os
import time
from typing import Dict, Optional, Set, Tuple

from morpher_runner.cgra_resources import get_op_resource_counts
from morpher_runner.dfg_graph import DFGGraph
from morpher_runner.exec import Command, run_command
from morpher_runner.mapper import resolve_arch_template_path
from morpher_runner.partitioner import PartitioningError, partition_dfg_ii1_recursive


def _read_mem_alloc_lines(path: str):
  """Return (header_line, list_of_body_lines) from a mem_alloc CSV file."""
  if not os.path.isfile(path):
    return None, []
  with open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()
  if not lines:
    return None, []
  return lines[0], lines[1:]


def _parse_mem_alloc_map(body_lines: list) -> Dict[str, int]:
  entries: Dict[str, int] = {}
  for line in body_lines:
    line = line.strip()
    if not line:
      continue
    parts = line.split(",")
    if len(parts) != 2:
      continue
    var_name = parts[0].strip()
    try:
      addr = int(parts[1].strip())
    except ValueError:
      continue
    if var_name:
      entries[var_name] = addr
  return entries


def _occupied_aligned_blocks(mem_alloc: Dict[str, int], total_size: int, align: int = 16) -> Set[int]:
  occupied: Set[int] = set()
  for addr in mem_alloc.values():
    block = (addr // align) * align
    for b in range(block, min(block + align * 2, total_size), align):
      occupied.add(b)
  return occupied


def _write_mem_alloc(path: str, header_line: str, body_lines: list, additions: dict) -> None:
  with open(path, "w", encoding="utf-8") as f:
    f.write(header_line if header_line else "var_name,base_addr\n")
    for line in body_lines:
      f.write(line)
    for var_name, addr in sorted(additions.items()):
      f.write("%s,%d\n" % (var_name, addr))


def _parse_result_file(result_path: str) -> Tuple[str, Optional[int]]:
  if not os.path.isfile(result_path):
    return "unknown", None

  with open(result_path, "r", encoding="utf-8") as f:
    lines = f.readlines()

  # Prefer the most recent mapper result entry.
  for line in reversed(lines):
    line = line.strip()
    if not line:
      continue
    parts = line.split()
    status = "unknown"
    ii = None
    for token in parts:
      if token.startswith("status:"):
        status = token[len("status:"):]
    if len(parts) >= 3:
      try:
        ii = int(parts[2])
      except (ValueError, IndexError):
        pass
    if status in ("success", "failed"):
      return status, ii

  return "unknown", None


def _parse_optional_int(value) -> Optional[int]:
  if value is None:
    return None
  if isinstance(value, int):
    return value
  text = str(value).strip().lower()
  if text == "auto":
    return None
  return int(text)


def _derive_load_store_limits(op_resources: Dict[str, int], load_cfg, store_cfg) -> Tuple[int, int]:
  load_limit = _parse_optional_int(load_cfg)
  store_limit = _parse_optional_int(store_cfg)

  mem_counts = []
  for op, cnt in op_resources.items():
    op_u = op.upper()
    if op_u.startswith("LOAD") or op_u.startswith("STORE") or op_u == "LS":
      mem_counts.append(cnt)
  default_limit = max(mem_counts) if mem_counts else 1

  if load_limit is None:
    load_limit = op_resources.get("LOAD", default_limit)
  if store_limit is None:
    store_limit = op_resources.get("STORE", default_limit)

  return max(1, int(load_limit)), max(1, int(store_limit))


def run_ii1_partition_mapper(context) -> None:
  """Partition DFG at II=1 and run the mapper on each partition in order."""
  config = context.config
  kernel = context.kernel

  print("\n=== II=1 Partitioned Mapping ===")

  original_xml = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % kernel)
  if not os.path.isfile(original_xml):
    raise RuntimeError(
      "DFG XML not found: %s\n"
      "Run dfg_gen_only or mapper_only first to stage the DFG XML, "
      "or provide an XML input." % original_xml
    )

  dfg = DFGGraph.from_xml(original_xml)
  print("Original DFG: %s  (%d nodes)" % (os.path.basename(original_xml), len(dfg.nodes)))

  arch_template = resolve_arch_template_path(context)
  op_resources = get_op_resource_counts(arch_template)

  all_counts = list(op_resources.values())
  compute_count = max(all_counts) if all_counts else 16
  mem_ops = {op: cnt for op, cnt in op_resources.items() if cnt < compute_count}
  resource_summary = "compute<=%d" % compute_count
  if mem_ops:
    mem_parts = ["%s<=%d" % (op, cnt) for op, cnt in sorted(mem_ops.items())]
    resource_summary = ", ".join(mem_parts) + ", " + resource_summary
  print("CGRA: %dx%d | Resource budget: %s" % (config.xdim, config.ydim, resource_summary))

  load_limit, store_limit = _derive_load_store_limits(
    op_resources=op_resources,
    load_cfg=config.ii1_partition_max_loads_per_chunk,
    store_cfg=config.ii1_partition_max_stores_per_chunk,
  )

  print(
    "Target partition size: %d nodes (min: %d) | balance_tolerance=%.2f | LOAD<=%d STORE<=%d"
    % (
      config.ii1_partition_target_size,
      config.ii1_partition_min_size,
      config.ii1_partition_balance_tolerance,
      load_limit,
      store_limit,
    )
  )

  original_mem_alloc_path = os.path.join(context.mapper_kernel, "%s_mem_alloc.txt" % kernel)
  orig_header, orig_body = _read_mem_alloc_lines(original_mem_alloc_path)
  if orig_header is None:
    orig_header = "var_name,base_addr\n"
    orig_body = []

  total_size = config.banksize * config.numberofbanks
  original_mem_alloc = _parse_mem_alloc_map(orig_body)
  occupied_addrs = _occupied_aligned_blocks(original_mem_alloc, total_size=total_size, align=16)

  # Try configured target first, then progressively larger targets to reduce cuts.
  base_target = max(1, int(config.ii1_partition_target_size))
  search_step = max(1, base_target // 4)
  candidate_targets = [base_target]
  t = base_target + search_step
  while t <= len(dfg.nodes):
    candidate_targets.append(t)
    t += search_step
  if candidate_targets[-1] != len(dfg.nodes):
    candidate_targets.append(len(dfg.nodes))

  last_error = None
  partitions = None
  chosen_target = None
  for target in candidate_targets:
    try:
      partitions = partition_dfg_ii1_recursive(
        dfg=dfg,
        op_resources=op_resources,
        target_size=target,
        min_target_size=min(config.ii1_partition_min_size, target),
        balance_tolerance=config.ii1_partition_balance_tolerance,
        max_loads_per_partition=load_limit,
        max_stores_per_partition=store_limit,
        element_size_bytes=4,
        total_size=total_size,
        bank_size=config.banksize,
        bank_number=config.numberofbanks,
        occupied_addrs=occupied_addrs,
      )
      chosen_target = target
      break
    except PartitioningError as exc:
      last_error = str(exc)

  if partitions is None:
    raise RuntimeError(
      "II=1 partitioning failed after trying target sizes %s. Last error: %s"
      % (candidate_targets, last_error if last_error else "unknown")
    )

  if chosen_target != config.ii1_partition_target_size:
    print("Selected target size after retries: %d (initial=%d)" % (
      chosen_target, config.ii1_partition_target_size
    ))

  print("Partitions created: %d\n" % len(partitions))

  results = []

  for part in partitions:
    part_dir = os.path.join(context.mapper_kernel, "partition_%d" % part.idx)
    os.makedirs(part_dir, exist_ok=True)

    part_kernel = "%s_part%d" % (kernel, part.idx)
    part_xml_name = "%s_PartPredDFG.xml" % part_kernel
    part_xml_path = os.path.join(part_dir, part_xml_name)

    part.dfg.to_xml(part_xml_path, reindex=True)

    hist = part.dfg.op_histogram()
    hist_str = " ".join("%s:%d" % (op, cnt) for op, cnt in sorted(hist.items()) if op != "NOP")
    print(
      "  Part %d: nodes=%d cross_in=%d cross_out=%d loads=%d/%d stores=%d/%d "
      "injLOAD=%d injSTORE=%d resMII=%d recMII=%d [%s]"
      % (
        part.idx,
        len(part.dfg.nodes),
        part.cross_edges_in,
        part.cross_edges_out,
        part.incoming_load_count,
        load_limit,
        part.outgoing_store_count,
        store_limit,
        part.injected_loads,
        part.injected_stores,
        part.res_mii,
        part.rec_mii,
        hist_str,
      ),
      end="",
      flush=True,
    )

    if config.memory_mode == "auto":
      part_mem_alloc_path = os.path.join(part_dir, "%s_mem_alloc.txt" % part_kernel)
      _write_mem_alloc(part_mem_alloc_path, orig_header, orig_body, part.mem_alloc_additions)

      part_arch_runtime = os.path.join(part_dir, "%s_arch_runtime.json" % part_kernel)
      update_cmd = Command(
        argv=[
          "python",
          "%s/update_mem_alloc.py" % context.mapper_home,
          arch_template,
          part_mem_alloc_path,
          str(config.banksize),
          str(config.numberofbanks),
          part_arch_runtime,
        ],
        cwd=part_dir,
      )
      mapper_arch = part_arch_runtime
    else:
      update_cmd = None
      mapper_arch = arch_template

    mapper_cmd = Command(
      argv=[
        "%s/build/src/cgra_xml_mapper" % context.mapper_home,
        "-j", mapper_arch,
        "-t", "HyCUBE_4REG",
        "-d", part_xml_name,
        "-x", str(config.xdim),
        "-y", str(config.ydim),
        "-i", "1",
        "-m", str(config.mapping_method),
        "-b", str(config.backtrack_limit),
        "-r", str(config.max_mapper_iter),
        "--max_II", "1",
        "--ii_restarts", str(config.ii_restarts),
        "--skip_ii_estimation", "0",
        "--print_dfg", "1" if config.print_dfg else "0",
        "--emit_binary", "0",
        "--sa_seed", str(config.sa_seed),
        "--sa_restarts", str(config.sa_restarts),
        "--sa_moves_per_temp", str(config.sa_moves_per_temp),
        "--sa_temp_max", str(config.sa_temp_max),
        "--sa_temp_min", str(config.sa_temp_min),
        "--sa_candidate_topk", str(config.sa_candidate_topk),
        "--spm_mode", str(config.spm_mode),
      ],
      cwd=part_dir,
    )

    start = time.time()
    try:
      if update_cmd is not None:
        run_command(update_cmd)
      run_command(mapper_cmd)
      elapsed = time.time() - start

      result_path = os.path.join(part_dir, "result.txt")
      status, achieved_ii = _parse_result_file(result_path)
      if status == "success" and achieved_ii == 1:
        print("  -> SUCCESS (II=1) [%.1fs]" % elapsed)
      elif status == "success":
        print("  -> MAPPED at II=%s (not II=1) [%.1fs]" % (achieved_ii, elapsed))
        status = "failed"
      else:
        print("  -> FAILED (II=%s, status=%s) [%.1fs]" % (achieved_ii, status, elapsed))

    except Exception as exc:
      elapsed = time.time() - start
      status = "error"
      achieved_ii = None
      print("  -> ERROR: %s [%.1fs]" % (exc, elapsed))

    results.append((part.idx, status, achieved_ii))

  succeeded = sum(1 for _, s, ii in results if s == "success" and ii == 1)
  attempted = len(results)
  print("\nResult: %d/%d mapped partitions achieved II=1" % (succeeded, attempted))
  print("Partitions created: %d" % len(partitions))

  if config.ii1_partition_hard_fail and succeeded != attempted:
    raise RuntimeError(
      "II=1 partitioned mapping failed: %d/%d partitions mapped at II=1."
      % (succeeded, attempted)
    )
