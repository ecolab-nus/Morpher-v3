import json
import csv
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from morpher_runner.artifact_simulator import build_artifact_sim_command
from morpher_runner.memory_profile import combine_profiles
from morpher_runner.memory_profile import profile_csv_fields
from scripts.compare_memory_profiles import discover_profiles, profile_rows
import run_morpher


def _profile(issues, peak, conflicts=0, excess=0):
  stats = {
    "issues": issues,
    "bytes": issues * 4,
    "unbound_issues": 0,
    "ambiguous_issues": 0,
    "port_conflict_cycles": conflicts,
    "excess_transactions": excess,
    "bank_capacity_excess_transactions": excess,
    "banks": [{
      "bank": "B0",
      "physical_ports": 2,
      "issues": issues,
      "bytes": issues * 4,
      "peak_concurrent": peak,
      "port_conflict_cycles": conflicts,
      "excess_transactions": excess,
      "bank_capacity_excess_transactions": excess,
    }],
  }
  return {
    "schema_version": 1,
    "kind": "morpher_memory_access_profile",
    "timing_effect": "observational_only",
    "phases": [{"phase": 0, "stats": stats}],
    "aggregate": stats,
  }


class MemoryProfileTests(unittest.TestCase):
  def test_direct_result_csv_includes_memory_counters(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = Path(tmp)
      config = root / "config.yaml"
      config.write_text("validation_mode: event_driven\n", encoding="utf-8")
      projected = {
        field: 0 for field in run_morpher.MEMORY_PROFILE_CSV_FIELDS
      }
      projected["memory_profile_file"] = "/tmp/memory_profile.json"
      projected["memory_profile_samples"] = 2
      projected["memory_issues"] = 17
      with mock.patch.object(
          run_morpher, "_estimate_result_utilization", return_value={}
      ), mock.patch.object(
          run_morpher, "profile_csv_fields", return_value=projected
      ):
        run_morpher._write_run_artifacts(
          root, "suite/kernel/kernel.c", "kernel", str(config),
          "unit_profile", "ok", 0, 1.0,
        )
      with (root / "result.csv").open(newline="", encoding="utf-8") as stream:
        row = next(csv.DictReader(stream))
      self.assertEqual(row["memory_profile_samples"], "2")
      self.assertEqual(row["memory_issues"], "17")

  def test_event_driven_command_requests_structured_profile(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = Path(tmp)
      mapping = root / "probe_II=1_mapping.mappingwithlatency.txt"
      mapping.write_text("mapping\n", encoding="utf-8")
      (root / "probe_mem_alloc.txt").write_text("a,0\n", encoding="utf-8")
      context = SimpleNamespace(
        kernel="probe",
        mapper_kernel=str(root),
        mem_trace=str(root),
      )
      profile = root / "profile.json"
      with mock.patch(
          "morpher_runner.artifact_simulator.resolve_artifact_simulator",
          return_value="/tmp/artifact_simulator",
      ):
        command = build_artifact_sim_command(
          context, "probe_trace_0.txt", "event_driven", profile
        )
      index = command.argv.index("--memory-profile-json")
      self.assertEqual(command.argv[index + 1], str(profile))

  def test_sample_aggregation_and_csv_projection(self):
    with tempfile.TemporaryDirectory() as tmp:
      root = Path(tmp)
      first = root / "first.json"
      second = root / "second.json"
      combined = root / "memory_profile.json"
      first.write_text(json.dumps(_profile(3, 1)), encoding="utf-8")
      second.write_text(json.dumps(_profile(5, 3, 2, 2)), encoding="utf-8")

      doc = combine_profiles(
        [first, second], combined, labels=["trace_0", "trace_1"]
      )
      self.assertEqual(doc["sample_count"], 2)
      self.assertEqual(doc["aggregate"]["issues"], 8)
      self.assertEqual(doc["aggregate"]["banks"][0]["peak_concurrent"], 3)
      fields = profile_csv_fields(combined)
      self.assertEqual(fields["memory_profile_samples"], 2)
      self.assertEqual(fields["memory_issues"], 8)
      self.assertEqual(fields["memory_peak_concurrent"], 3)
      self.assertEqual(fields["memory_port_conflict_cycles"], 2)

  def test_comparison_discovers_profiles_recursively(self):
    with tempfile.TemporaryDirectory() as tmp:
      profile = Path(tmp) / "variant_a" / "memory_profile.json"
      profile.parent.mkdir()
      profile.write_text(json.dumps(_profile(4, 2)), encoding="utf-8")
      paths = discover_profiles([tmp])
      self.assertEqual(paths, [profile])
      rows = profile_rows(paths)
      self.assertEqual(rows[0]["label"], "variant_a")
      self.assertEqual(rows[0]["memory_issues"], 4)


if __name__ == "__main__":
  unittest.main()
