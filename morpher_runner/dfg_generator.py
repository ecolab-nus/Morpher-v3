import json
import os
import re
import time
import xml.etree.ElementTree as ET
from pathlib import Path

from morpher_runner.exec import Command
from morpher_runner.exec import run_command
from morpher_runner.exec import run_commands
from morpher_runner.fs_ops import copy_file
from morpher_runner.fs_ops import remove_glob
from morpher_runner.simd_packer import filter_trace_vars
from morpher_runner.simd_packer import remap_agi_subgraph_externalidx
from morpher_runner.simd_packer import reindex_addr_trace
from morpher_runner.simd_packer import remap_addr_trace
from morpher_runner.simd_packer import SimdMapping
from morpher_runner.simd_packer import BoundaryMapping
from morpher_runner.utils import build_clang_ir_command
from morpher_runner.utils import BankSizeTooSmallError
from morpher_runner.utils import build_llvm_tool_env
from morpher_runner.utils import resolve_llvm_tool
from morpher_runner.utils import run_dfg_opt_with_diagnostics


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


def _read_unwrapped_xml(path):
  with open(path, "r", encoding="utf-8") as stream:
    text = stream.read()
  return ET.fromstring("<Root>\n%s\n</Root>" % text)


def _classify_staged_dfg_nodes(xml_path):
  root = _read_unwrapped_xml(xml_path)
  dfg = root.find("DFG")
  if dfg is None:
    raise RuntimeError("DFG XML missing <DFG> root in %s" % xml_path)

  counts = {
    "total": 0,
    "scalar_compute": 0,
    "vector_compute": 0,
    "memory_ops": 0,
  }
  for node in dfg.findall("Node"):
    counts["total"] += 1
    op_elem = node.find("OP")
    op = (op_elem.text or "").strip() if op_elem is not None and op_elem.text else ""
    if not op:
      continue
    if op.startswith(MEMORY_OP_PREFIXES):
      counts["memory_ops"] += 1
    elif op.startswith("V"):
      counts["vector_compute"] += 1
    else:
      counts["scalar_compute"] += 1
  return counts


def _print_staged_dfg_summary(xml_path):
  counts = _classify_staged_dfg_nodes(xml_path)
  print(
    "Staged DFG summary: total_nodes=%d, scalar_compute=%d, vector_compute=%d, memory_ops=%d\n"
    % (
      counts["total"],
      counts["scalar_compute"],
      counts["vector_compute"],
      counts["memory_ops"],
    )
  )


def count_trace_files(trace_dir, kernel):
  pattern = re.compile(r"^%s_trace_[0-9]+\.txt$" % re.escape(kernel))
  count = 0
  for filename in os.listdir(trace_dir):
    if pattern.match(filename):
      count += 1
  return count


def build_ir_generation_commands(context):
  kernel = context.kernel
  opt_tool = resolve_llvm_tool("opt")
  return [
    build_clang_ir_command(
      context.csourcefile,
      "%s.ll" % kernel,
      define_cgra_compiler=True,
      suppress_frontend_warnings=True,
      cwd=context.dfg_gen_kernel,
    ),
    Command(
      argv=[
        opt_tool,
        "-passes=default<O2>",
        "%s.ll" % kernel,
        "-S",
        "-o",
        "%s_opt.ll" % kernel,
      ],
      cwd=context.dfg_gen_kernel,
      env=build_llvm_tool_env(),
    ),
  ]


def simd_packing_active_for_factor(config, unroll_factor):
  return (
    config.simd_pack == "verified"
    and config.arch_simd_enabled
    and unroll_factor >= config.arch_simd_width
  )


def build_loop_interchange_command(context):
  kernel = context.kernel
  return Command(
    argv=[
      resolve_llvm_tool("opt"),
      "-load-pass-plugin",
      "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
      "-passes=morpher-loop-interchange",
      "--morpher-unroll-fn",
      kernel,
      "-pass-remarks=loop-interchange",
      "-pass-remarks-missed=loop-interchange",
      "%s_opt.ll" % kernel,
      "-S",
      "-o",
      "%s_li_opt.ll" % kernel,
    ],
    cwd=context.dfg_gen_kernel,
    env=build_llvm_tool_env(),
  )


def build_unroll_command(context, unroll_factor, input_ir=None):
  kernel = context.kernel
  if input_ir is None:
    input_ir = "%s_opt.ll" % kernel
  simd_active = simd_packing_active_for_factor(context.config, unroll_factor)
  if simd_active:
    reduction_lanes = context.config.arch_simd_width
    reduction_policy = (
      "vector-psum"
      if "VRED" in context.config.arch_vector_ops
      else "packed-psum"
    )
    if reduction_policy == "vector-psum" and unroll_factor % reduction_lanes != 0:
      raise RuntimeError(
        "Reduction-axis vector-psum requires unroll factor %d to be an integer "
        "multiple of SIMD width %d; partial vector chunks are not implemented."
        % (unroll_factor, reduction_lanes)
      )
  elif context.config.auto_unroll_scalar_reduction_policy == "none":
    reduction_lanes = 1
    reduction_policy = "tree"
  else:
    reduction_lanes = context.config.auto_unroll_scalar_reduction_terms
    if unroll_factor > 1 and unroll_factor % reduction_lanes != 0:
      raise RuntimeError(
        "auto_unroll.scalar_reduction_terms=%d must divide unroll factor %d."
        % (reduction_lanes, unroll_factor)
      )
    reduction_policy = context.config.auto_unroll_scalar_reduction_policy
  opt_tool = resolve_llvm_tool("opt")
  command = Command(
    argv=[
      opt_tool,
      "-load-pass-plugin",
      "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
      "-passes=morpher-loop-unroll",
      "--morpher-unroll-fn",
      kernel,
      "--morpher-unroll-factor",
      str(unroll_factor),
      "--morpher-promote-memory-reductions",
      "--morpher-reduction-lanes",
      str(reduction_lanes),
      "--morpher-reduction-policy",
      reduction_policy,
      input_ir,
      "-S",
      "-o",
      "%s_uf%d_opt.ll" % (kernel, unroll_factor),
    ],
    cwd=context.dfg_gen_kernel,
    env=build_llvm_tool_env(),
  )
  if context.config.auto_unroll_fuse_innermost_loops:
    command.argv.insert(3, "--morpher-fuse-innermost-loops")
  return command


def build_systolic_unroll_jam_command(
  context, candidate_index, factor0, factor1, input_ir=None,
  dataflow="output_stationary", schedule="direct",
):
  kernel = context.kernel
  if input_ir is None:
    input_ir = "%s_opt.ll" % kernel
  output_ir = "%s_sj_c%d_%dx%d_opt.ll" % (
    kernel, candidate_index, factor0, factor1
  )
  if schedule == "direct":
    pass_pipeline = "morpher-systolic-unroll-jam"
  elif schedule == "polly_distribute_beta":
    pass_pipeline = (
      "morpher-systolic-schedule-prep,"
      "morpher-systolic-polly-distribute,"
      "simplifycfg,adce,simplifycfg,"
      "morpher-systolic-unroll-jam"
    )
  else:
    raise ValueError("unsupported systolic schedule: %s" % schedule)
  argv = [
      resolve_llvm_tool("opt"),
      "-load-pass-plugin",
      "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
      "-passes=%s" % pass_pipeline,
      "-morpher-systolic-fn",
      kernel,
      "-morpher-systolic-candidate-index",
      str(candidate_index),
      "-morpher-systolic-factor0",
      str(factor0),
      "-morpher-systolic-factor1",
      str(factor1),
      "-morpher-systolic-dataflow",
      dataflow,
      input_ir,
      "-S",
      "-o",
      output_ir,
    ]
  if schedule == "polly_distribute_beta":
    argv.insert(4, "-polly-only-func=%s" % kernel)
  config = getattr(context, "config", None)
  fp_nondeterminism = getattr(config, "auto_unroll_fp_nondeterminism", True)
  argv.insert(
    len(argv) - 4,
    "-morpher-systolic-fp-nondeterminism=%s"
    % ("true" if fp_nondeterminism else "false"),
  )
  return Command(
    argv=argv,
    cwd=context.dfg_gen_kernel,
    env=build_llvm_tool_env(),
  )


def build_dfg_pass_command(context, skeleton_ir=None):
  config = context.config
  kernel = context.kernel
  if skeleton_ir is None:
    skeleton_ir = "%s_opt.ll" % kernel
  cmd = [
    resolve_llvm_tool("opt"),
    "-load-pass-plugin",
    "%s/build/src/libdfggenPass.so" % context.dfg_gen_home,
    "-passes=skeleton",
    "--fn",
    kernel,
    "--nobanks",
    str(config.numberofbanks),
    "--banksize",
    str(config.banksize),
    "-S",
    skeleton_ir,
    "-o",
    "%s_opt_instrument.ll" % kernel,
  ]
  if config.externalize_access != "off":
    cmd.append("--externalize-access=%s" % config.externalize_access)
  if config.simd_pack == "verified" and getattr(context, "selected_simd_enabled", False):
    cmd.append("--simd-pack=%d" % config.arch_simd_width)
    cmd.append("--simd-vector-ops=%s" % ",".join(sorted(config.arch_vector_ops)))
    cmd.append("--simd-scalar-broadcast=%d" % (1 if config.arch_scalar_broadcast else 0))
  if config.boundary_canonicalization == "verified":
    cmd.append("--canonicalize-loop-boundaries")
    if config.arch_native_mac:
      cmd.append("--native-stateful-mac")
    if config.treg_reset_on_launch:
      cmd.append("--treg-reset-on-launch")
  if config.suppress_dfg_dot:
    cmd.append("--suppress-dfg-dot")
  return cmd


def _can_auto_grow_banksize(context):
  if context.config.memory_mode == "auto":
    return True
  return str(context.config.spm_mode).strip().lower() != "on"


def _run_dfg_pass_with_banksize_retry(context, skeleton_ir):
  config = context.config
  kernel = context.kernel
  dfg_kernel = context.dfg_gen_kernel
  original_banksize = config.banksize
  max_attempts = 8
  last_error = None

  for attempt in range(1, max_attempts + 1):
    if attempt > 1:
      print(
        "Retrying DFG generation with larger banksize=%d bytes (attempt %d/%d).\n"
        % (config.banksize, attempt, max_attempts)
      )
    try:
      diagnostics = run_dfg_opt_with_diagnostics(
        build_dfg_pass_command(context, skeleton_ir=skeleton_ir),
        kernel,
        config.banksize,
        config.numberofbanks,
        cwd=dfg_kernel,
      )
      required_banksize = diagnostics.get("required_banksize")
      if required_banksize and required_banksize > config.banksize:
        if not _can_auto_grow_banksize(context):
          raise RuntimeError(
            "Provisional DFG layout requires banksize=%d bytes, but memory_mode=%s with spm_mode=%s "
            "cannot auto-grow the architecture at mapper time."
            % (required_banksize, context.config.memory_mode, context.config.spm_mode)
          )
        config.banksize = required_banksize
      if config.banksize != original_banksize:
        print(
          "DFG bank allocation required banksize=%d bytes; continuing with that effective size.\n"
          % config.banksize
        )
      return
    except BankSizeTooSmallError as exc:
      last_error = exc
      if not _can_auto_grow_banksize(context):
        print(
          "Auto-growing banksize is disabled for memory_mode=%s with spm_mode=%s because the selected "
          "static architecture JSON would become inconsistent.\n"
          % (context.config.memory_mode, context.config.spm_mode)
        )
        raise
      next_banksize = exc.required_banksize or (config.banksize * 2)
      if next_banksize <= config.banksize:
        next_banksize = config.banksize + 1
      config.banksize = next_banksize

  if last_error is not None:
    raise last_error


def validate_dfg_outputs(context):
  kernel = context.kernel
  expected = [
    "%s_PartPredDFG.xml" % kernel,
    "%s_mem_alloc.txt" % kernel,
    "%s_opt_instrument.ll" % kernel,
  ]
  if not context.config.suppress_dfg_dot:
    expected.insert(0, "%s_PartPredDFG.dot" % kernel)

  missing = []
  for filename in expected:
    path = "%s/%s" % (context.dfg_gen_kernel.rstrip("/"), filename)
    try:
      with open(path, "r", encoding="utf-8", errors="ignore"):
        pass
    except OSError:
      missing.append(filename)

  if missing:
    raise RuntimeError(
      "DFG generation step finished without required output files: %s. "
      "This can happen when memory-bank allocation fails; increase banksize/numberofbanks."
      % ", ".join(missing)
    )


def validate_agi_manifest(context):
  manifest_paths = sorted(Path(context.dfg_gen_kernel).glob("*_AGI_manifest.json"))
  if not manifest_paths:
    raise RuntimeError(
      "Access externalization requested, but DFG generation did not emit any *_AGI_manifest.json files under %s"
      % context.dfg_gen_kernel
    )

  for manifest_path in manifest_paths:
    _validate_one_agi_manifest(manifest_path)
  return manifest_paths


def validate_iter_manifest(context):
  manifest_paths = sorted(Path(context.dfg_gen_kernel).glob("*_ITER_manifest.json"))
  if len(manifest_paths) != 1:
    raise RuntimeError(
      "agi_iter requires exactly one *_ITER_manifest.json under %s; found %d."
      % (context.dfg_gen_kernel, len(manifest_paths))
    )
  manifest_path = manifest_paths[0]
  try:
    with manifest_path.open("r", encoding="utf-8") as stream:
      manifest = json.load(stream)
  except (OSError, json.JSONDecodeError) as exc:
    raise RuntimeError("Could not read iterator manifest %s: %s" % (manifest_path, exc))

  status = manifest.get("status")
  if (manifest.get("schema_version") != 1 or
      manifest.get("transform") != "externalize_iterator_only" or
      status not in ("applied", "skipped")):
    raise RuntimeError("Iterator manifest %s has an unsupported schema or status." % manifest_path)

  original_count = manifest.get("original_node_count")
  removed_count = manifest.get("removed_node_count")
  final_count = manifest.get("final_node_count")
  if (not isinstance(original_count, int) or original_count < 1 or
      not isinstance(removed_count, int) or removed_count < 0 or
      not isinstance(final_count, int) or final_count < 1 or
      original_count != removed_count + final_count):
    raise RuntimeError("Iterator manifest %s has inconsistent node counts." % manifest_path)

  removed_nodes = manifest.get("removed_nodes")
  if (not isinstance(removed_nodes, list) or len(removed_nodes) != removed_count or
      any(not isinstance(index, int) for index in removed_nodes) or
      len(set(removed_nodes)) != removed_count or
      set(removed_nodes) - set(range(original_count))):
    raise RuntimeError("Iterator manifest %s has invalid removed_nodes." % manifest_path)

  reindex = {}
  for entry in manifest.get("index_map", []):
    if not isinstance(entry, dict):
      raise RuntimeError("Iterator manifest %s has an invalid index-map entry." % manifest_path)
    original_idx = entry.get("original_idx")
    final_idx = entry.get("final_idx")
    if (not isinstance(original_idx, int) or not isinstance(final_idx, int) or
        original_idx in reindex or original_idx in removed_nodes or
        original_idx < 0 or original_idx >= original_count or
        final_idx < 0 or final_idx >= final_count):
      raise RuntimeError("Iterator manifest %s has inconsistent index mappings." % manifest_path)
    reindex[original_idx] = final_idx
  if set(reindex) | set(removed_nodes) != set(range(original_count)):
    raise RuntimeError("Iterator manifest %s does not cover its original index space." % manifest_path)
  if set(reindex.values()) != set(range(final_count)):
    raise RuntimeError("Iterator manifest %s final index space is not contiguous." % manifest_path)

  if status == "skipped":
    if removed_count != 0 or reindex != {index: index for index in range(original_count)}:
      raise RuntimeError("Skipped iterator manifest %s must contain an identity mapping." % manifest_path)
    reason = manifest.get("reason")
    if not isinstance(reason, str) or not reason:
      raise RuntimeError("Skipped iterator manifest %s is missing its reason." % manifest_path)
    print("Iterator externalization skipped conservatively: %s\n" % reason)

  # In a combined verified-SIMD path, the other manifest determines whether
  # this is an intermediate or final index space. Cross-manifest validation is
  # performed after both files have been read.
  config = getattr(context, "config", None)
  if not (
      getattr(config, "simd_pack", "off") == "verified"
      and getattr(context, "selected_simd_enabled", False)
  ):
    dfg_path = Path(context.dfg_gen_kernel) / ("%s_PartPredDFG.xml" % context.kernel)
    root = _read_unwrapped_xml(dfg_path)
    dfg = root.find("DFG")
    if dfg is None:
      raise RuntimeError("Emitted DFG XML is missing <DFG>: %s" % dfg_path)
    xml_indices = {int(node.attrib["idx"]) for node in dfg.findall("Node")}
    if xml_indices != set(range(final_count)):
      raise RuntimeError("Iterator manifest %s does not match emitted DFG XML." % manifest_path)

  artifact_vars = manifest.get("artifact_vars_pruned", [])
  if (not isinstance(artifact_vars, list) or
      any(not isinstance(name, str) for name in artifact_vars)):
    raise RuntimeError("Iterator manifest %s has invalid artifact_vars_pruned." % manifest_path)

  print(
    "Iterator manifest verified: %s removed_nodes=%d, remaining_nodes=%d\n"
    % (manifest_path.name, removed_count, final_count)
  )
  return reindex, tuple(sorted(set(artifact_vars)))


def _validate_one_agi_manifest(manifest_path):
  try:
    with manifest_path.open("r", encoding="utf-8") as stream:
      manifest = json.load(stream)
  except (OSError, json.JSONDecodeError) as exc:
    raise RuntimeError("Could not read AGI manifest %s: %s" % (manifest_path, exc))

  if manifest.get("schema_version") != 1:
    raise RuntimeError("AGI manifest %s has unsupported schema_version." % manifest_path)
  if manifest.get("transform") != "externalize_agi":
    raise RuntimeError("AGI manifest %s has unexpected transform." % manifest_path)

  nodes = manifest.get("nodes")
  if not isinstance(nodes, list):
    raise RuntimeError("AGI manifest %s is missing nodes list." % manifest_path)
  removed_count = int(manifest.get("removed_node_count", -1))
  remaining_count = int(manifest.get("remaining_node_count", -1))
  original_count = int(manifest.get("original_node_count", -1))
  if removed_count != len(nodes):
    raise RuntimeError(
      "AGI manifest %s removed_node_count=%d but contains %d node records."
      % (manifest_path, removed_count, len(nodes))
    )
  if original_count != removed_count + remaining_count:
    raise RuntimeError(
      "AGI manifest %s has inconsistent counts: original=%d removed=%d remaining=%d."
      % (manifest_path, original_count, removed_count, remaining_count)
    )

  seen_local = set()
  boundary_inputs = 0
  boundary_outputs = 0
  for node in nodes:
    if not isinstance(node, dict):
      raise RuntimeError("AGI manifest %s has a non-object node record." % manifest_path)
    local_idx = node.get("local_idx")
    if not isinstance(local_idx, int) or local_idx < 0:
      raise RuntimeError("AGI manifest %s has invalid local_idx=%r." % (manifest_path, local_idx))
    if local_idx in seen_local:
      raise RuntimeError("AGI manifest %s repeats local_idx=%d." % (manifest_path, local_idx))
    seen_local.add(local_idx)

    for field, counter_name in (("inputs", "input"), ("outputs", "output")):
      refs = node.get(field)
      if not isinstance(refs, list):
        raise RuntimeError("AGI manifest %s node %d missing %s list." % (manifest_path, local_idx, field))
      for ref in refs:
        if not isinstance(ref, dict):
          raise RuntimeError("AGI manifest %s node %d has invalid %s ref." % (manifest_path, local_idx, field))
        kind = ref.get("kind")
        if kind == "agi":
          if not isinstance(ref.get("local_idx"), int):
            raise RuntimeError("AGI manifest %s has AGI ref without local_idx." % manifest_path)
        elif kind == "external":
          external_idx = ref.get("external_idx")
          if not isinstance(external_idx, int) or external_idx < 0 or external_idx >= remaining_count:
            raise RuntimeError("AGI manifest %s has invalid external_idx=%r." % (manifest_path, external_idx))
          if counter_name == "input":
            boundary_inputs += 1
          else:
            boundary_outputs += 1
        else:
          raise RuntimeError("AGI manifest %s has invalid ref kind=%r." % (manifest_path, kind))

  if seen_local != set(range(removed_count)):
    raise RuntimeError("AGI manifest %s local_idx values are not contiguous." % manifest_path)
  if int(manifest.get("boundary_input_count", -1)) != boundary_inputs:
    raise RuntimeError("AGI manifest %s boundary_input_count is inconsistent." % manifest_path)
  if int(manifest.get("boundary_output_count", -1)) != boundary_outputs:
    raise RuntimeError("AGI manifest %s boundary_output_count is inconsistent." % manifest_path)

  print(
    "AGI manifest verified: %s removed_nodes=%d, remaining_nodes=%d, boundary_inputs=%d, boundary_outputs=%d\n"
    % (manifest_path.name, removed_count, remaining_count, boundary_inputs, boundary_outputs)
  )


def validate_simd_manifest(context):
  manifest_paths = sorted(Path(context.dfg_gen_kernel).glob("*_SIMD_manifest.json"))
  if len(manifest_paths) != 1:
    raise RuntimeError(
      "Verified SIMD packing requires exactly one *_SIMD_manifest.json under %s; found %d."
      % (context.dfg_gen_kernel, len(manifest_paths))
    )
  manifest_path = manifest_paths[0]
  try:
    with manifest_path.open("r", encoding="utf-8") as stream:
      manifest = json.load(stream)
  except (OSError, json.JSONDecodeError) as exc:
    raise RuntimeError("Could not read SIMD manifest %s: %s" % (manifest_path, exc))

  if manifest.get("schema_version") != 1 or manifest.get("transform") != "vector_pack":
    raise RuntimeError("SIMD manifest %s has an unsupported schema." % manifest_path)
  if manifest.get("status") != "applied":
    raise RuntimeError("SIMD manifest %s did not apply cleanly." % manifest_path)
  post_pack_iterator_externalized = manifest.get(
    "post_pack_iterator_externalized", False
  )
  if not isinstance(post_pack_iterator_externalized, bool):
    raise RuntimeError(
      "SIMD manifest %s has invalid post_pack_iterator_externalized."
      % manifest_path
    )

  original_count = manifest.get("original_node_count")
  final_count = manifest.get("final_node_count")
  lanes = manifest.get("lanes")
  if not isinstance(original_count, int) or original_count < 0:
    raise RuntimeError("SIMD manifest %s has invalid original_node_count." % manifest_path)
  if not isinstance(final_count, int) or final_count < 0:
    raise RuntimeError("SIMD manifest %s has invalid final_node_count." % manifest_path)
  if lanes != context.config.arch_simd_width:
    raise RuntimeError(
      "SIMD manifest %s uses %r lanes, expected architecture width %d."
      % (manifest_path, lanes, context.config.arch_simd_width)
    )

  dfg_path = Path(context.dfg_gen_kernel) / ("%s_PartPredDFG.xml" % context.kernel)
  root = _read_unwrapped_xml(dfg_path)
  dfg = root.find("DFG")
  if dfg is None:
    raise RuntimeError("Emitted DFG XML is missing <DFG>: %s" % dfg_path)
  xml_nodes = {int(node.attrib["idx"]): node for node in dfg.findall("Node")}
  xml_indices = set(xml_nodes)
  if xml_indices != set(range(final_count)):
    raise RuntimeError(
      "SIMD manifest %s final index space does not match emitted XML." % manifest_path
    )

  reindex = {}
  for entry in manifest.get("index_map", []):
    if not isinstance(entry, dict):
      raise RuntimeError("SIMD manifest %s has an invalid index-map entry." % manifest_path)
    original_idx = entry.get("original_idx")
    final_idx = entry.get("final_idx")
    if (not isinstance(original_idx, int) or not isinstance(final_idx, int) or
        original_idx in reindex or final_idx not in xml_indices):
      raise RuntimeError("SIMD manifest %s has inconsistent index mappings." % manifest_path)
    reindex[original_idx] = final_idx
  pruned_nodes = manifest.get("pruned_nodes", [])
  if (not isinstance(pruned_nodes, list) or
      any(not isinstance(idx, int) for idx in pruned_nodes) or
      len(set(pruned_nodes)) != len(pruned_nodes) or
      any(idx < 0 or idx >= original_count for idx in pruned_nodes) or
      set(pruned_nodes) & set(reindex) or
      set(reindex) | set(pruned_nodes) != set(range(original_count))):
    raise RuntimeError("SIMD manifest %s has invalid pruned_nodes coverage." % manifest_path)

  synthesized_indices = set()
  for entry in manifest.get("synthesized_nodes", []):
    if not isinstance(entry, dict):
      raise RuntimeError("SIMD manifest %s has an invalid synthesized node." % manifest_path)
    final_idx = entry.get("final_idx")
    op = entry.get("op")
    xml_node = xml_nodes.get(final_idx) if isinstance(final_idx, int) else None
    op_elem = xml_node.find("OP") if xml_node is not None else None
    xml_op = (op_elem.text or "").strip() if op_elem is not None else ""
    if (not isinstance(final_idx, int) or final_idx in synthesized_indices or
        final_idx not in xml_indices or not isinstance(op, str) or op != xml_op):
      raise RuntimeError("SIMD manifest %s has inconsistent synthesized nodes." % manifest_path)
    synthesized_indices.add(final_idx)

  if set(reindex.values()) | synthesized_indices != xml_indices:
    raise RuntimeError("SIMD manifest %s does not cover every final node." % manifest_path)

  artifact_vars_pruned = manifest.get("pruned_artifact_vars", [])
  if (not isinstance(artifact_vars_pruned, list) or
      any(not isinstance(name, str) or not name for name in artifact_vars_pruned)):
    raise RuntimeError("SIMD manifest %s has invalid pruned_artifact_vars." % manifest_path)

  simd_groups = {}
  covered_lanes = set()
  for group in manifest.get("packed_groups", []):
    if not isinstance(group, dict):
      raise RuntimeError("SIMD manifest %s has an invalid packed group." % manifest_path)
    final_idx = group.get("final_idx")
    lane_indices = group.get("lanes")
    if (not isinstance(final_idx, int) or final_idx not in xml_indices or
        not isinstance(lane_indices, list) or len(lane_indices) != lanes):
      raise RuntimeError("SIMD manifest %s has an invalid packed group shape." % manifest_path)
    if (final_idx in simd_groups or any(not isinstance(idx, int) for idx in lane_indices) or
        any(idx in covered_lanes or reindex.get(idx) != final_idx for idx in lane_indices)):
      raise RuntimeError("SIMD manifest %s has inconsistent lane mappings." % manifest_path)
    simd_groups[final_idx] = lane_indices
    covered_lanes.update(lane_indices)

  exit_reduction_sources = set()
  boundary_trace_vars = set()
  expected_vector_updates = {
    "ADD": "VADD",
    "AND": "VAND",
    "OR": "VOR",
    "XOR": "VXOR",
  }
  for entry in manifest.get("exit_reductions", []):
    if not isinstance(entry, dict):
      raise RuntimeError("SIMD manifest %s has an invalid exit reduction." % manifest_path)
    source_idx = entry.get("source_final_idx")
    reduce_op = entry.get("reduce_op")
    source_node = xml_nodes.get(source_idx) if isinstance(source_idx, int) else None
    source_op_elem = source_node.find("OP") if source_node is not None else None
    source_op = (source_op_elem.text or "").strip() if source_op_elem is not None else ""
    scalar_init_trace_var = entry.get("scalar_init_trace_var")
    scalar_exit_trace_var = entry.get("scalar_exit_trace_var")
    lane_seed_trace_vars = entry.get("lane_seed_trace_vars")
    lane_trace_vars = entry.get("lane_trace_vars")
    has_self_recurrence = source_node is not None and any(
      output.get("idx") == str(source_idx) and
      output.get("nextiter") == "1" and output.get("type") == "I1"
      for output in source_node.findall("./Outputs/Output")
    )
    if (
        not isinstance(source_idx, int) or source_idx in exit_reduction_sources or
        source_idx not in simd_groups or reduce_op not in expected_vector_updates or
        source_op != expected_vector_updates[reduce_op] or
        source_node.get("simd_accumulator_op") != reduce_op or
        entry.get("op") != "VRED" or entry.get("execution") != "loop_exit" or
        entry.get("accumulator_identity") != (
          "all_ones" if reduce_op == "AND" else "zero"
        ) or entry.get("accumulator_init_execution") != "loop_preheader" or
        entry.get("accumulator_init_scheduled") is not False or
        not isinstance(scalar_init_trace_var, str) or not scalar_init_trace_var or
        not isinstance(scalar_exit_trace_var, str) or not scalar_exit_trace_var or
        not isinstance(lane_seed_trace_vars, list) or
        len(lane_seed_trace_vars) != lanes or
        any(not isinstance(name, str) or not name for name in lane_seed_trace_vars) or
        len(set(lane_seed_trace_vars)) != lanes or
        not isinstance(lane_trace_vars, list) or len(lane_trace_vars) != lanes or
        any(not isinstance(name, str) or not name for name in lane_trace_vars) or
        len(set(lane_trace_vars)) != lanes or
        source_node.get("simd_accumulator_init_var") != scalar_init_trace_var or
        source_node.get("simd_accumulator_exit_var") != scalar_exit_trace_var or
        source_node.get("simd_accumulator_seed_var") != lane_seed_trace_vars[0] or
        entry.get("combines_scalar_initializer") is not True or
        entry.get("stores_result") is not True or
        entry.get("scheduled") is not False or not has_self_recurrence):
      raise RuntimeError(
        "SIMD manifest %s has an inconsistent exit reduction contract."
        % manifest_path
      )
    exit_reduction_sources.add(source_idx)
    boundary_trace_vars.update(lane_seed_trace_vars)
    boundary_trace_vars.update(lane_trace_vars)
    boundary_trace_vars.add(scalar_init_trace_var)
    boundary_trace_vars.add(scalar_exit_trace_var)

  print(
    "SIMD manifest verified: %s packed_groups=%d, exit_reductions=%d, "
    "original_nodes=%d, final_nodes=%d\n"
    % (
      manifest_path.name,
      len(simd_groups),
      len(exit_reduction_sources),
      original_count,
      final_count,
    )
  )
  if exit_reduction_sources:
    print(
      "Mapping scope: steady-state loop DFG only.\n"
      "CGRA-unscheduled boundary work: vector-accumulator preheader initialization; "
      "exit VRED, scalar combine, and result store.\n"
      "Simulation contract: seed recurrence lanes and validate final lanes/exit "
      "using retained source-trace values.\n"
    )
  return SimdMapping(
    reindex=reindex,
    simd_groups=simd_groups,
    simd_splat_groups=frozenset(),
    simd_reduced_store_groups=frozenset(),
    simd_no_vector_word_groups=frozenset(),
    artifact_vars_pruned=frozenset(set(artifact_vars_pruned) - boundary_trace_vars),
    post_pack_iterator_externalized=post_pack_iterator_externalized,
    boundary_trace_vars=frozenset(boundary_trace_vars),
    host_modeled_boundaries=len(exit_reduction_sources),
  )


def validate_boundary_manifest(context):
  manifest_paths = sorted(Path(context.dfg_gen_kernel).glob("*_BOUNDARY_manifest.json"))
  if not manifest_paths:
    return None
  if len(manifest_paths) != 1:
    raise RuntimeError(
      "Boundary canonicalization requires at most one *_BOUNDARY_manifest.json "
      "under %s; found %d." % (context.dfg_gen_kernel, len(manifest_paths))
    )
  manifest_path = manifest_paths[0]
  try:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
  except (OSError, json.JSONDecodeError) as exc:
    raise RuntimeError("Could not read boundary manifest %s: %s" % (manifest_path, exc))
  if (manifest.get("schema_version") != 2 or
      manifest.get("transform") != "loop_boundary_canonicalization" or
      manifest.get("status") != "applied"):
    raise RuntimeError("Boundary manifest %s has an unsupported schema." % manifest_path)

  launch = manifest.get("launch")
  actions = launch.get("actions") if isinstance(launch, dict) else None
  if (not isinstance(launch, dict) or
      launch.get("execution") != "before_each_loop_invocation" or
      not isinstance(actions, list)):
    raise RuntimeError("Boundary manifest %s has an invalid launch contract." % manifest_path)
  reset_actions = [
    action for action in actions
    if isinstance(action, dict) and action == {
      "operation": "reset",
      "storage": "treg",
      "scope": "all_pes",
      "value": 0,
    }
  ]
  if len(reset_actions) != len(actions) or len(reset_actions) > 1:
    raise RuntimeError("Boundary manifest %s has invalid launch actions." % manifest_path)
  resets_treg = bool(reset_actions)

  dfg_path = Path(context.dfg_gen_kernel) / ("%s_PartPredDFG.xml" % context.kernel)
  root = _read_unwrapped_xml(dfg_path)
  dfg = root.find("DFG")
  if dfg is None:
    raise RuntimeError("Emitted DFG XML is missing <DFG>: %s" % dfg_path)
  xml_nodes = {int(node.attrib["idx"]): node for node in dfg.findall("Node")}
  if manifest.get("final_node_count") != len(xml_nodes) or set(xml_nodes) != set(range(len(xml_nodes))):
    raise RuntimeError("Boundary manifest %s does not match the final XML index space." % manifest_path)

  original_count = manifest.get("original_node_count")
  reindex = {}
  for entry in manifest.get("index_map", []):
    if not isinstance(entry, dict):
      raise RuntimeError("Boundary manifest %s has an invalid index map." % manifest_path)
    original_idx = entry.get("original_idx")
    final_idx = entry.get("final_idx")
    if (not isinstance(original_idx, int) or original_idx in reindex or
        not isinstance(final_idx, int) or final_idx not in xml_nodes):
      raise RuntimeError("Boundary manifest %s has an inconsistent index map." % manifest_path)
    reindex[original_idx] = final_idx
  pruned_nodes = manifest.get("pruned_nodes")
  if (not isinstance(original_count, int) or original_count < 0 or
      not isinstance(pruned_nodes, list) or
      any(not isinstance(idx, int) for idx in pruned_nodes) or
      len(set(pruned_nodes)) != len(pruned_nodes) or
      set(pruned_nodes) & set(reindex) or
      set(pruned_nodes) | set(reindex) != set(range(original_count)) or
      set(reindex.values()) != set(xml_nodes)):
    raise RuntimeError("Boundary manifest %s has invalid original-node coverage." % manifest_path)

  trace_vars = set()
  seen_nodes = set()
  has_treg_state = False
  states = manifest.get("states")
  if not isinstance(states, list) or not states:
    raise RuntimeError("Boundary manifest %s has no state entries." % manifest_path)
  for state in states:
    if not isinstance(state, dict):
      raise RuntimeError("Boundary manifest %s has an invalid state entry." % manifest_path)
    node_idx = state.get("mapped_node")
    lanes = state.get("lanes")
    bits = state.get("value_bits")
    storage = state.get("storage")
    node = xml_nodes.get(node_idx) if isinstance(node_idx, int) else None
    op_elem = node.find("OP") if node is not None else None
    op = (op_elem.text or "").strip() if op_elem is not None else ""
    seed = state.get("seed")
    drain = state.get("drain")
    if (node is None or node_idx in seen_nodes or lanes != 1 or bits != 32 or
        storage not in ("explicit_recurrence", "treg") or
        node.get("loop_carried_state") != "true" or
        node.get("state_storage") != storage or
        not isinstance(seed, dict) or not isinstance(drain, dict) or
        seed.get("execution") != "loop_launch" or seed.get("scheduled") is not False or
        drain.get("execution") != "loop_exit" or drain.get("scheduled") is not False):
      raise RuntimeError("Boundary manifest %s has an inconsistent state contract." % manifest_path)
    final_vars = drain.get("trace_vars")
    if (not isinstance(final_vars, list) or len(final_vars) != lanes or
        any(not isinstance(name, str) or not name for name in final_vars)):
      raise RuntimeError("Boundary manifest %s has invalid drain trace variables." % manifest_path)
    if storage == "explicit_recurrence":
      has_self_recurrence = any(
        output.get("idx") == str(node_idx) and output.get("nextiter") == "1" and
        output.get("type") == "I1"
        for output in node.findall("./Outputs/Output")
      )
      if seed.get("kind") != "constant" or not isinstance(seed.get("value"), int) or not has_self_recurrence:
        raise RuntimeError("Boundary manifest %s has an invalid explicit recurrence seed." % manifest_path)
    else:
      if (op != "MAC" or seed.get("kind") != "register_reset" or
          seed.get("value") != 0):
        raise RuntimeError("Boundary manifest %s has an invalid TREG state contract." % manifest_path)
      has_treg_state = True
    trace_vars.update(final_vars)
    for name in seed.get("trace_vars", []):
      if not isinstance(name, str) or not name:
        raise RuntimeError("Boundary manifest %s has invalid seed trace variables." % manifest_path)
      trace_vars.add(name)
    seen_nodes.add(node_idx)

  if has_treg_state != resets_treg:
    raise RuntimeError(
      "Boundary manifest %s TREG state disagrees with its launch actions."
      % manifest_path
    )

  pruned_vars = manifest.get("pruned_artifact_vars", [])
  if (not isinstance(pruned_vars, list) or
      any(not isinstance(name, str) or not name for name in pruned_vars)):
    raise RuntimeError("Boundary manifest %s has invalid pruned_artifact_vars." % manifest_path)
  print(
    "Boundary manifest verified: %s states=%d final_nodes=%d\n"
    "Mapping scope: steady-state loop DFG only; register reset/seeding and "
    "live-out drains are CGRA-unscheduled boundary actions.\n"
    % (manifest_path.name, len(states), len(xml_nodes))
  )
  return BoundaryMapping(
    reindex=reindex,
    artifact_vars_pruned=frozenset(set(pruned_vars) - trace_vars),
    boundary_trace_vars=frozenset(trace_vars),
    state_count=len(states),
  )


def build_instrumentation_commands(context):
  kernel = context.kernel
  clang_tool = resolve_llvm_tool("clang")
  llvm_link_tool = resolve_llvm_tool("llvm-link")
  llc_tool = resolve_llvm_tool("llc")
  clangxx_tool = resolve_llvm_tool("clang++")
  commands = [
    Command(
      argv=[
        clang_tool,
        "-target",
        "i386-unknown-linux-gnu",
        "-c",
        "-emit-llvm",
        "-S",
        "%s/src/instrumentation/instrumentation.cpp" % context.dfg_gen_home,
        "-o",
        "instrumentation.ll",
      ],
      cwd=context.dfg_gen_kernel,
      env=build_llvm_tool_env(),
    )
  ]

  if kernel == "kernel_symm":
    commands.append(
      build_clang_ir_command(
        "polybench.c",
        "polybench.ll",
        define_cgra_compiler=True,
        cwd=context.dfg_gen_kernel,
      )
    )
    commands.append(
      Command(
        argv=[llvm_link_tool, "%s_opt_instrument.ll" % kernel, "instrumentation.ll", "polybench.ll", "-o", "final.ll"],
        cwd=context.dfg_gen_kernel,
        env=build_llvm_tool_env(),
      )
    )
  else:
    commands.append(
      Command(
        argv=[llvm_link_tool, "%s_opt_instrument.ll" % kernel, "instrumentation.ll", "-o", "final.ll"],
        cwd=context.dfg_gen_kernel,
        env=build_llvm_tool_env(),
      )
    )

  commands.extend(
    [
      Command(argv=[llc_tool, "-filetype=obj", "final.ll", "-o", "final.o"], cwd=context.dfg_gen_kernel, env=build_llvm_tool_env()),
      Command(argv=[clangxx_tool, "-m32", "-no-pie", "final.o", "-o", "final"], cwd=context.dfg_gen_kernel, env=build_llvm_tool_env()),
    ]
  )
  return commands


def _default_unroll_factor(config):
  if config.auto_unroll_enabled:
    return sorted(config.auto_unroll_factors, reverse=True)[0]
  return 1


def _unroll_pass_required(config, unroll_factor):
  return unroll_factor > 1


def run_dfg_generator(
  context, unroll_factor=None, interchange_loops=False, systolic_jam=None,
  systolic_schedule="direct", systolic_dataflow="output_stationary",
):
  config = context.config
  kernel = context.kernel
  dfg_kernel = context.dfg_gen_kernel
  if unroll_factor is None:
    unroll_factor = _default_unroll_factor(config)
  if systolic_jam is not None:
    if unroll_factor != 1 or interchange_loops:
      raise RuntimeError(
        "Systolic unroll-and-jam cannot be combined with scalar unroll or "
        "loop-interchange candidates."
      )
    if len(systolic_jam) != 3:
      raise RuntimeError(
        "systolic_jam must be (candidate_index, factor0, factor1)."
      )
  simd_active = simd_packing_active_for_factor(config, unroll_factor)
  verified_simd_active = simd_active and config.simd_pack == "verified"
  context.selected_simd_enabled = simd_active

  print("-----Running Morpher_DFG_Generator-----\n")
  if config.arch_simd_enabled:
    if simd_active:
      print(
        "Architecture-driven SIMD packing enabled for this candidate: width=%d, scalar_broadcast=%s\n"
        % (config.arch_simd_width, "true" if config.arch_scalar_broadcast else "false")
      )
    else:
      print(
        "SIMD hardware enabled, but this candidate is narrower than SIMD_WIDTH=%d; "
        "mapping scalar lanes without SIMD packing.\n"
        % config.arch_simd_width
      )
  if unroll_factor > 1:
    print(
      "Requested Morpher scalar loop unroll: factor=%d (remainders preserved; not SIMD width)\n"
      % unroll_factor
    )
  if config.auto_unroll_fuse_innermost_loops and config.auto_unroll_enabled:
    print(
      "Requested Morpher innermost loop fusion where LLVM can prove legality.\n"
    )
  if interchange_loops:
    print(
      "Requested LLVM loop interchange candidate for the uniquely annotated perfect nest.\n"
    )
  if systolic_jam is not None:
    candidate_index, factor0, factor1 = systolic_jam
    print(
      "Requested systolic unroll-and-jam: candidate=%d factors=%dx%d.\n"
      % (candidate_index, factor0, factor1)
    )
  if (
      unroll_factor > 1
      and not simd_active
      and config.auto_unroll_scalar_reduction_policy != "none"):
    print(
      "Requested scalar reduction lowering: policy=%s terms=%d\n"
      % (
        config.auto_unroll_scalar_reduction_policy,
        config.auto_unroll_scalar_reduction_terms,
      )
    )

  remove_glob(dfg_kernel, "memtraces/*")
  remove_glob(dfg_kernel, "*.xml")
  remove_glob(dfg_kernel, "*.dot")

  ir_commands = build_ir_generation_commands(context)

  print("Generating IR..\n")
  run_command(ir_commands[0])

  print("Optimizing IR..\n")
  run_command(ir_commands[1])

  skeleton_ir = "%s_opt.ll" % kernel
  if interchange_loops:
    print("Trying legal/profitable loop interchange before unrolling..\n")
    run_command(build_loop_interchange_command(context))
    skeleton_ir = "%s_li_opt.ll" % kernel
  if systolic_jam is not None:
    candidate_index, factor0, factor1 = systolic_jam
    print("Applying detected systolic unroll-and-jam recipe..\n")
    command = build_systolic_unroll_jam_command(
      context, candidate_index, factor0, factor1, skeleton_ir,
      dataflow=systolic_dataflow, schedule=systolic_schedule,
    )
    run_command(command)
    skeleton_ir = "%s_sj_c%d_%dx%d_opt.ll" % (
      kernel, candidate_index, factor0, factor1
    )
  if _unroll_pass_required(config, unroll_factor):
    print("Unrolling annotated loop for DFG candidate..\n")
    run_command(build_unroll_command(context, unroll_factor, skeleton_ir))
    skeleton_ir = "%s_uf%d_opt.ll" % (kernel, unroll_factor)

  dfg_artifact_label = "xml" if config.suppress_dfg_dot else "xml/dot"
  print("Generating DFG (%s_PartPredDFG.%s) and data layout (%s_mem_alloc.txt)..\n" % (kernel, dfg_artifact_label, kernel))
  _run_dfg_pass_with_banksize_retry(context, skeleton_ir)

  validate_dfg_outputs(context)
  if config.externalize_access != "off":
    validate_agi_manifest(context)
  iter_reindex = {}
  iter_artifact_vars_pruned = ()
  if config.externalize_access == "agi_iter":
    iter_reindex, iter_artifact_vars_pruned = validate_iter_manifest(context)
  simd_mapping = validate_simd_manifest(context) if verified_simd_active else None
  boundary_mapping = (
    validate_boundary_manifest(context)
    if config.boundary_canonicalization == "verified" else None
  )
  if boundary_mapping is not None:
    remap_agi_subgraph_externalidx(
      Path(context.dfg_gen_kernel), boundary_mapping.reindex
    )
  elif iter_reindex:
    agi_reindex = iter_reindex
    if simd_mapping is not None:
      if simd_mapping.post_pack_iterator_externalized:
        if not set(iter_reindex.values()).issuperset(
            set(simd_mapping.reindex.values())):
          raise RuntimeError(
            "Post-pack iterator and SIMD manifests have incompatible final index spaces."
          )
        agi_reindex = simd_mapping.reindex
      else:
        if set(iter_reindex.values()) != set(simd_mapping.reindex):
          raise RuntimeError(
            "Iterator and SIMD manifests use incompatible intermediate index spaces."
          )
        agi_reindex = {
          before_iter: simd_mapping.reindex[after_iter]
          for before_iter, after_iter in iter_reindex.items()
        }
    # AGI boundary references are emitted before iterator removal (and SIMD,
    # when enabled). This is metadata remapping only; the main DFG XML is
    # already final C++ output.
    remap_agi_subgraph_externalidx(Path(context.dfg_gen_kernel), agi_reindex)

  if config.generate_dfg_pdf and not config.suppress_dfg_dot:
    run_command(Command(
      argv=["dot", "-Tpdf", "%s_PartPredDFG.dot" % kernel, "-o", "%s_PartPredDFG.pdf" % kernel],
      cwd=dfg_kernel,
    ))
  elif not config.generate_dfg_pdf:
    print("DFG PDF generation disabled (generate_dfg_pdf=false).\n")
  else:
    print("DFG PDF generation skipped because DOT emission is disabled.\n")
  dfg_xml = "%s/%s_PartPredDFG.xml" % (dfg_kernel.rstrip("/"), kernel)
  mapper_xml = dfg_xml
  simd_artifact_vars_pruned = (
    tuple(sorted(simd_mapping.artifact_vars_pruned))
    if simd_mapping is not None else ()
  )
  boundary_artifact_vars_pruned = (
    tuple(sorted(boundary_mapping.artifact_vars_pruned))
    if boundary_mapping is not None else ()
  )
  copy_file(mapper_xml, context.mapper_kernel)
  if mapper_xml != dfg_xml:
    staged_xml = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % kernel)
    packed_name = os.path.join(context.mapper_kernel, os.path.basename(mapper_xml))
    if packed_name != staged_xml:
      os.replace(packed_name, staged_xml)
  staged_xml = os.path.join(context.mapper_kernel, "%s_PartPredDFG.xml" % kernel)
  _print_staged_dfg_summary(staged_xml)
  copy_file("%s/%s_mem_alloc.txt" % (dfg_kernel.rstrip("/"), kernel), context.mapper_kernel)
  # Stage the AGI sub-graph next to the DFG XML so the layout solver can find it.
  for agi_path in Path(dfg_kernel).glob("*_AGI_subgraph.xml"):
    copy_file(str(agi_path), context.mapper_kernel)
  for agi_manifest_path in Path(dfg_kernel).glob("*_AGI_manifest.json"):
    copy_file(str(agi_manifest_path), context.mapper_kernel)
  for iter_manifest_path in Path(dfg_kernel).glob("*_ITER_manifest.json"):
    copy_file(str(iter_manifest_path), context.mapper_kernel)
  for simd_manifest_path in Path(dfg_kernel).glob("*_SIMD_manifest.json"):
    copy_file(str(simd_manifest_path), context.mapper_kernel)
  for boundary_manifest_path in Path(dfg_kernel).glob("*_BOUNDARY_manifest.json"):
    copy_file(str(boundary_manifest_path), context.mapper_kernel)
  remove_glob(dfg_kernel, "*.log")

  if not config.enable_simulation:
    print("Simulation disabled (enable_simulation=false): skipping code instrumentation and trace generation.\n")
    remove_glob(dfg_kernel, "*.ll")
    return


  print("\nCode instrumentation..\n")
  run_commands(build_instrumentation_commands(context))

  trace_limit = int(config.max_test_samples)
  if trace_limit < 0:
    raise RuntimeError("max_test_samples must be >= 0, got %d" % trace_limit)

  trace_env = os.environ.copy()
  trace_env["MORPHER_MAX_TRACES"] = str(trace_limit)
  print("Trace generation limit (MORPHER_MAX_TRACES): %d" % trace_limit)
  print("Running instrumented code to generate the data memory content (memtraces/%s_trace_x.txt)..\n" % kernel)
  start_time = time.time()
  with open("%s/final_log.txt" % dfg_kernel.rstrip("/"), "w", encoding="utf-8") as stdout_log:
    with open("%s/final_err_log.txt" % dfg_kernel.rstrip("/"), "w", encoding="utf-8") as stderr_log:
      result = run_command(Command(
        argv=["./final"],
        cwd=dfg_kernel,
        stdout=stdout_log,
        stderr=stderr_log,
        check=False,
        env=trace_env,
      )
      )
  elapsed = time.time() - start_time
  trace_count = count_trace_files("%s/memtraces" % dfg_kernel.rstrip("/"), kernel)
  print("Instrumentation run complete in %.2f sec. Generated %d trace file(s)." % (elapsed, trace_count))
  if result.returncode != 0:
    printable_code = result.returncode if result.returncode >= 0 else (128 + (-result.returncode))
    print("Warning: instrumented binary ./final exited with code %d; simulation traces will not be available." % printable_code)
  elif trace_count == 0 and unroll_factor > 1:
    print(
      "Warning: 0 trace files generated despite a clean instrumented run.\n"
      "  The unroll factor (%d) likely exceeds the runtime loop trip count: the\n"
      "  unrolled body never executes, only the scalar remainder. Lower\n"
      "  `auto_unroll.factors` (e.g. [4] or [2]) or use a larger benchmark.\n"
      "  Downstream simulation will be skipped." % unroll_factor
    )
  else:
    trace_zero = "%s/memtraces/%s_trace_0.txt" % (dfg_kernel.rstrip("/"), kernel)
    if os.path.isfile(trace_zero):
      copy_file(trace_zero, context.simulator_kernel)
    else:
      print("Warning: no %s_trace_0.txt found; continuing with available trace files." % kernel)
  copy_file("%s/%s_mem_alloc.txt" % (dfg_kernel.rstrip("/"), kernel), context.simulator_kernel)

  if boundary_mapping is not None:
    import glob as _glob
    addr_trace_dir = "%s/memtraces" % dfg_kernel.rstrip("/")
    pruned_trace_vars = set(iter_artifact_vars_pruned) | set(boundary_artifact_vars_pruned)
    for trace_path in sorted(_glob.glob("%s/%s_trace_*.txt" % (addr_trace_dir, kernel))):
      filter_trace_vars(trace_path, trace_path, pruned_trace_vars)
    for addr_trace_path in sorted(_glob.glob("%s/%s_addr_trace_*.txt" % (addr_trace_dir, kernel))):
      reindex_addr_trace(addr_trace_path, addr_trace_path, boundary_mapping.reindex)
  elif iter_reindex:
    import glob as _glob
    addr_trace_dir = "%s/memtraces" % dfg_kernel.rstrip("/")
    for trace_path in sorted(_glob.glob("%s/%s_trace_*.txt" % (addr_trace_dir, kernel))):
      filter_trace_vars(trace_path, trace_path, iter_artifact_vars_pruned)
    if not (
        simd_mapping is not None and
        simd_mapping.post_pack_iterator_externalized):
      for addr_trace_path in sorted(_glob.glob("%s/%s_addr_trace_*.txt" % (addr_trace_dir, kernel))):
        reindex_addr_trace(addr_trace_path, addr_trace_path, iter_reindex)
  if simd_mapping is not None:
    import glob as _glob
    addr_trace_dir = "%s/memtraces" % dfg_kernel.rstrip("/")
    if simd_artifact_vars_pruned:
      for trace_path in sorted(_glob.glob("%s/%s_trace_*.txt" % (addr_trace_dir, kernel))):
        filter_trace_vars(trace_path, trace_path, simd_artifact_vars_pruned)
    for addr_trace_path in sorted(_glob.glob("%s/%s_addr_trace_*.txt" % (addr_trace_dir, kernel))):
      remap_addr_trace(addr_trace_path, addr_trace_path, simd_mapping)

  remove_glob(dfg_kernel, "*.ll")
