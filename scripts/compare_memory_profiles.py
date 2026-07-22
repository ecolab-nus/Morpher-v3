#!/usr/bin/env python3
"""Compare one or more Morpher memory_profile.json artifacts."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
  sys.path.insert(0, str(_REPO_ROOT))

from morpher_runner.memory_profile import profile_csv_fields


DISPLAY_FIELDS = [
  "memory_profile_samples",
  "memory_issues",
  "memory_bytes",
  "memory_accessed_banks",
  "memory_peak_concurrent",
  "memory_port_conflict_cycles",
  "memory_excess_transactions",
  "memory_bank_capacity_excess_transactions",
  "memory_unbound_issues",
  "memory_ambiguous_issues",
]


def discover_profiles(inputs):
  found = {}
  for raw in inputs:
    path = Path(raw)
    if path.is_file():
      found[path.resolve()] = path
    elif path.is_dir():
      for profile in path.rglob("memory_profile.json"):
        found[profile.resolve()] = profile
    else:
      raise RuntimeError("memory-profile input does not exist: %s" % path)
  return [found[key] for key in sorted(found, key=str)]


def profile_rows(paths):
  rows = []
  for path in paths:
    fields = profile_csv_fields(path)
    parts = path.parts
    label = path.parent.name
    if "morpher_runs" in parts:
      index = parts.index("morpher_runs")
      if index + 1 < len(parts):
        label = "%s:%s" % (parts[index + 1], path.parent.name)
    row = {
      "label": label,
      "profile": str(path),
    }
    row.update({field: fields[field] for field in DISPLAY_FIELDS})
    rows.append(row)
  return rows


def print_rows(rows):
  headings = [
    ("label", 28),
    ("memory_profile_samples", 7),
    ("memory_issues", 9),
    ("memory_accessed_banks", 7),
    ("memory_peak_concurrent", 7),
    ("memory_port_conflict_cycles", 10),
    ("memory_excess_transactions", 8),
    ("memory_unbound_issues", 8),
    ("memory_ambiguous_issues", 9),
  ]
  aliases = {
    "memory_profile_samples": "samples",
    "memory_issues": "issues",
    "memory_accessed_banks": "banks",
    "memory_peak_concurrent": "peak",
    "memory_port_conflict_cycles": "conflicts",
    "memory_excess_transactions": "excess",
    "memory_unbound_issues": "unbound",
    "memory_ambiguous_issues": "ambiguous",
  }
  print(" ".join(aliases.get(field, field).ljust(width) for field, width in headings))
  for row in rows:
    print(" ".join(str(row.get(field, "")).ljust(width) for field, width in headings))


def main(argv=None):
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
    "inputs", nargs="+",
    help="Profile files or directories recursively containing memory_profile.json",
  )
  parser.add_argument("--output-csv", type=Path)
  args = parser.parse_args(argv)

  paths = discover_profiles(args.inputs)
  if not paths:
    raise RuntimeError("no memory_profile.json artifacts found")
  rows = profile_rows(paths)
  print_rows(rows)
  if args.output_csv:
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    fields = ["label", "profile"] + DISPLAY_FIELDS
    with args.output_csv.open("w", newline="", encoding="utf-8") as stream:
      writer = csv.DictWriter(stream, fieldnames=fields)
      writer.writeheader()
      writer.writerows(rows)
  return 0


if __name__ == "__main__":
  try:
    sys.exit(main())
  except RuntimeError as exc:
    print("Error: %s" % exc, file=sys.stderr)
    sys.exit(1)
