"""Remap addr_trace files after the C++ layout solver has rewritten data
placement. The solver emits *_layout_decision.json with a `node_addr_delta`
mapping (DFG node idx -> bytes to add to the trace's `addr` column for that
node's accesses). Trace rows for untouched nodes pass through unchanged.

Slot this between mapper and simulator in morpher_runner/mapper.py. When the
decision file is absent (solver disabled or did not mutate), the function
no-ops.
"""
from __future__ import annotations

import json
from pathlib import Path

from morpher_runner.log import get_logger

_LOG = get_logger(__name__)


def _find_decision_file(search_dir: Path) -> Path | None:
  if not search_dir.exists():
    return None
  for entry in sorted(search_dir.iterdir()):
    name = entry.name
    if name.endswith("_layout_decision.json"):
      return entry
  return None


def remap_addr_traces(memtraces_dir: str | Path, decision_path: str | Path | None = None) -> int:
  """Apply node_addr_delta to every *_addr_trace_*.txt in memtraces_dir.

  Returns the number of trace rows rewritten. Writes the files in place,
  preserving the original line layout (only `addr` column is mutated).
  """
  memtraces = Path(memtraces_dir)
  if not memtraces.exists():
    return 0

  if decision_path is None:
    decision_path = _find_decision_file(memtraces.parent)
  else:
    decision_path = Path(decision_path)
    if not decision_path.exists():
      decision_path = _find_decision_file(memtraces.parent)

  if decision_path is None:
    return 0

  with open(decision_path, encoding="utf-8") as f:
    decision = json.load(f)

  raw_delta = decision.get("node_addr_delta", {}) or {}
  if not raw_delta:
    return 0
  node_delta: dict[int, int] = {int(k): int(v) for k, v in raw_delta.items() if int(v) != 0}
  if not node_delta:
    return 0

  rewritten = 0
  for trace_path in sorted(memtraces.glob("*addr_trace*.txt")):
    new_lines = []
    changed = False
    with open(trace_path, encoding="utf-8") as f:
      for line in f:
        stripped = line.rstrip("\n")
        if not stripped or stripped.startswith("node_idx"):
          new_lines.append(line)
          continue
        parts = stripped.split(",")
        if len(parts) < 3:
          new_lines.append(line)
          continue
        try:
          node_idx = int(parts[0])
        except ValueError:
          new_lines.append(line)
          continue
        delta = node_delta.get(node_idx)
        if delta is None:
          new_lines.append(line)
          continue
        try:
          addr = int(parts[2])
        except ValueError:
          new_lines.append(line)
          continue
        parts[2] = str(addr + delta)
        new_lines.append(",".join(parts) + "\n")
        changed = True
        rewritten += 1
    if changed:
      with open(trace_path, "w", encoding="utf-8") as f:
        f.writelines(new_lines)
      _LOG.info(f"[trace_partition] Rewrote {trace_path.name} ({rewritten} rows so far)")
  return rewritten


if __name__ == "__main__":
  import sys
  if len(sys.argv) < 2:
    _LOG.info("usage: trace_partition.py <memtraces_dir> [decision_json]")
    sys.exit(1)
  remap_addr_traces(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
