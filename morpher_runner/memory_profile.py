import json
from pathlib import Path


PROFILE_KIND = "morpher_memory_access_profile"
PROFILE_SCHEMA_VERSION = 1
CSV_FIELDS = [
  "memory_profile_file",
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


def _empty_stats():
  return {
    "issues": 0,
    "bytes": 0,
    "unbound_issues": 0,
    "ambiguous_issues": 0,
    "port_conflict_cycles": 0,
    "excess_transactions": 0,
    "bank_capacity_excess_transactions": 0,
    "banks": [],
  }


def validate_profile(doc, source="memory profile"):
  if not isinstance(doc, dict):
    raise RuntimeError("%s must be a JSON object" % source)
  if doc.get("schema_version") != PROFILE_SCHEMA_VERSION:
    raise RuntimeError("%s has unsupported schema_version" % source)
  if doc.get("kind") != PROFILE_KIND:
    raise RuntimeError("%s has unexpected kind" % source)
  aggregate = doc.get("aggregate")
  if not isinstance(aggregate, dict) or not isinstance(aggregate.get("banks"), list):
    raise RuntimeError("%s lacks aggregate memory statistics" % source)
  return doc


def load_profile(path):
  path = Path(path)
  try:
    doc = json.loads(path.read_text(encoding="utf-8"))
  except (OSError, ValueError) as exc:
    raise RuntimeError("could not read memory profile %s: %s" % (path, exc))
  return validate_profile(doc, str(path))


def merge_stats(stats_list):
  merged = _empty_stats()
  banks = {}
  summed = (
    "issues",
    "bytes",
    "unbound_issues",
    "ambiguous_issues",
    "port_conflict_cycles",
    "excess_transactions",
    "bank_capacity_excess_transactions",
  )
  for stats in stats_list:
    for field in summed:
      merged[field] += int(stats.get(field, 0))
    for bank in stats.get("banks", []):
      name = str(bank.get("bank", ""))
      if not name:
        continue
      out = banks.setdefault(name, {
        "bank": name,
        "physical_ports": 0,
        "issues": 0,
        "bytes": 0,
        "peak_concurrent": 0,
        "port_conflict_cycles": 0,
        "excess_transactions": 0,
        "bank_capacity_excess_transactions": 0,
      })
      out["physical_ports"] = max(
        out["physical_ports"], int(bank.get("physical_ports", 0))
      )
      out["peak_concurrent"] = max(
        out["peak_concurrent"], int(bank.get("peak_concurrent", 0))
      )
      for field in (
        "issues", "bytes", "port_conflict_cycles", "excess_transactions",
        "bank_capacity_excess_transactions",
      ):
        out[field] += int(bank.get(field, 0))
  merged["banks"] = [banks[name] for name in sorted(banks)]
  return merged


def combine_profiles(profile_paths, output_path, labels=None):
  paths = [Path(path) for path in profile_paths]
  if not paths:
    raise RuntimeError("cannot combine an empty memory-profile list")
  if labels is None:
    labels = [path.stem for path in paths]
  if len(labels) != len(paths):
    raise RuntimeError("memory-profile labels and paths differ in length")

  samples = []
  aggregates = []
  for label, path in zip(labels, paths):
    doc = load_profile(path)
    aggregates.append(doc["aggregate"])
    samples.append({
      "label": str(label),
      "phases": doc.get("phases", []),
      "aggregate": doc["aggregate"],
    })
  combined = {
    "schema_version": PROFILE_SCHEMA_VERSION,
    "kind": PROFILE_KIND,
    "timing_effect": "observational_only",
    "sample_count": len(samples),
    "samples": samples,
    "aggregate": merge_stats(aggregates),
  }
  output_path = Path(output_path)
  output_path.parent.mkdir(parents=True, exist_ok=True)
  output_path.write_text(json.dumps(combined, indent=2) + "\n", encoding="utf-8")
  return combined


def profile_csv_fields(path):
  empty = {field: "" for field in CSV_FIELDS}
  path = Path(path)
  if not path.is_file():
    return empty
  doc = load_profile(path)
  stats = doc["aggregate"]
  banks = stats.get("banks", [])
  empty.update({
    "memory_profile_file": str(path),
    "memory_profile_samples": int(doc.get("sample_count", 1)),
    "memory_issues": int(stats.get("issues", 0)),
    "memory_bytes": int(stats.get("bytes", 0)),
    "memory_accessed_banks": sum(int(bank.get("issues", 0)) > 0 for bank in banks),
    "memory_peak_concurrent": max(
      [int(bank.get("peak_concurrent", 0)) for bank in banks] or [0]
    ),
    "memory_port_conflict_cycles": int(stats.get("port_conflict_cycles", 0)),
    "memory_excess_transactions": int(stats.get("excess_transactions", 0)),
    "memory_bank_capacity_excess_transactions": int(
      stats.get("bank_capacity_excess_transactions", 0)
    ),
    "memory_unbound_issues": int(stats.get("unbound_issues", 0)),
    "memory_ambiguous_issues": int(stats.get("ambiguous_issues", 0)),
  })
  return empty
