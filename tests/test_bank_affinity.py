import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

from morpher_runner.context import RunnerConfig, build_context
from morpher_runner.mapper import build_mapper_commands


class BankAffinityTests(unittest.TestCase):
  def _write_config(self, directory, value):
    config = yaml.safe_load(
        Path("config/regression.yaml").read_text(encoding="utf-8")
    )
    config["bank_affinity_cost"] = value
    path = Path(directory) / "bank_affinity.yaml"
    path.write_text(yaml.safe_dump(config), encoding="utf-8")
    return path

  def test_runner_passes_enabled_cost_to_mapper(self):
    with tempfile.TemporaryDirectory() as tmp:
      config_path = self._write_config(tmp, 125)
      context = build_context(
          "cgrame_microbench/conv2/conv2.c",
          "conv2",
          str(config_path),
          "mapper_only",
          run_tag="unit_bank_affinity",
      )
      commands, _ = build_mapper_commands(context)
      mapper_argv = commands[-1].argv
      idx = mapper_argv.index("--bank_affinity_cost")
      self.assertEqual(mapper_argv[idx + 1], "125")

  def test_runner_rejects_negative_cost(self):
    with tempfile.TemporaryDirectory() as tmp:
      config_path = self._write_config(tmp, -1)
      with self.assertRaisesRegex(RuntimeError, "non-negative integer"):
        RunnerConfig.from_yaml(str(config_path))

  def test_normalized_penalty_and_deterministic_tie(self):
    compiler = shutil.which("g++")
    if compiler is None:
      self.skipTest("g++ is required for the bank-affinity contract test")

    repo = Path(__file__).resolve().parents[1]
    include_dir = repo / "Morpher_CGRA_Mapper/include"
    with tempfile.TemporaryDirectory() as tmp:
      source = Path(tmp) / "bank_affinity_contract.cpp"
      binary = Path(tmp) / "bank_affinity_contract"
      source.write_text(
          r'''
#include "morpher/mapper/BankAffinity.h"
#include <map>
#include <string>

int main() {
  using namespace CGRAXMLCompile;
  std::map<std::string, int> counts{{"SPM_B1", 2}, {"SPM_B0", 2}};
  if (dominantBankKey(counts) != "SPM_B0") return 1;
  if (bankAffinityPenalty(counts, "SPM_B0", 125) != 0) return 2;
  if (bankAffinityPenalty(counts, "SPM_B1", 125) != 125) return 3;
  counts["SPM_B1"] = 100;
  if (bankAffinityPenalty(counts, "SPM_B0", 125) != 125) return 4;
  if (bankAffinityPenalty(counts, "SPM_B1", 125) != 0) return 5;
  if (bankAffinityPenalty(counts, "SPM_B0", 0) != 0) return 6;
  return 0;
}
''',
          encoding="utf-8",
      )
      subprocess.run(
          [compiler, "-std=c++17", "-I", str(include_dir), str(source), "-o", str(binary)],
          check=True,
          capture_output=True,
          text=True,
      )
      subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
  unittest.main()
