import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from morpher_runner.context import RunnerConfig
from morpher_runner.dfg_generator import build_dfg_pass_command
from morpher_runner.dfg_generator import validate_iter_manifest
from morpher_runner.mapper import build_mapper_commands
from morpher_runner.simd_packer import remap_addr_trace
from morpher_runner.simd_packer import SimdMapping


REPO_ROOT = Path(__file__).resolve().parents[2]


class ExternalizeAccessConfigTests(unittest.TestCase):

  def test_agi_pipeline_is_accepted(self):
    self.assertEqual(
      RunnerConfig._parse_externalize_access(
        {"dfg_pipeline": {"externalize_access": "agi"}}
      ),
      "agi",
    )

  def test_legacy_remove_agi_is_an_alias(self):
    self.assertEqual(
      RunnerConfig._parse_externalize_access({"remove_agi": True}),
      "agi",
    )

  def test_agi_iter_is_accepted_for_the_restricted_control_only_mode(self):
    self.assertEqual(
      RunnerConfig._parse_externalize_access(
        {"dfg_pipeline": {"externalize_access": "agi_iter"}}
      ),
      "agi_iter",
    )

  def test_pipeline_does_not_combine_with_legacy_iterator_pruning(self):
    with self.assertRaisesRegex(RuntimeError, "cannot be combined"):
      RunnerConfig._parse_externalize_access(
        {
          "dfg_pipeline": {"externalize_access": "agi"},
          "remove_iter": True,
        }
      )

  def test_verified_simd_requires_agi_externalization(self):
    with self.assertRaisesRegex(RuntimeError, "requires"):
      RunnerConfig._parse_simd_pack(
        {"dfg_pipeline": {"simd_pack": "verified"}},
        "off",
      )


class TraceDrivenCommandTests(unittest.TestCase):

  @classmethod
  def setUpClass(cls):
    cls.config = RunnerConfig.from_yaml(
      str(REPO_ROOT / "config" / "scalar_agi_pruned.yaml")
    )

  def test_dfg_generator_uses_the_verified_access_flag(self):
    context = SimpleNamespace(
      config=self.config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
    )
    command = build_dfg_pass_command(context)
    self.assertIn("--externalize-access=agi", command)
    self.assertNotIn("--remove-agi", command)

  def test_dfg_generator_passes_the_iterator_externalization_flag(self):
    config = RunnerConfig.from_yaml(
      str(REPO_ROOT / "config" / "scalar_agi_iter_pruned.yaml")
    )
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
    )
    command = build_dfg_pass_command(context)
    self.assertIn("--externalize-access=agi_iter", command)

  def test_mapper_binary_emission_is_disabled(self):
    context = SimpleNamespace(
      config=self.config,
      kernel="probe",
      morpher_home=str(REPO_ROOT),
      mapper_home=str(REPO_ROOT / "Morpher_CGRA_Mapper"),
      mapper_kernel="/tmp",
      selected_simd_artifact_only=False,
    )
    commands, _ = build_mapper_commands(context)
    mapper_command = commands[-1].argv
    emit_binary_index = mapper_command.index("--emit_binary")
    self.assertEqual(mapper_command[emit_binary_index + 1], "0")

  def test_verified_simd_adds_cxx_pass_flags(self):
    config = RunnerConfig.from_yaml(str(REPO_ROOT / "config" / "simd4_verified.yaml"))
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
      selected_simd_enabled=True,
    )
    command = build_dfg_pass_command(context)
    self.assertIn("--simd-pack=4", command)
    self.assertTrue(any(arg.startswith("--simd-vector-ops=") for arg in command))

  def test_manifest_style_simd_mapping_collapses_contiguous_trace_lanes(self):
    mapping = SimdMapping(
      reindex={10: 3, 11: 3, 12: 3, 13: 3},
      simd_groups={3: [10, 11, 12, 13]},
    )
    with tempfile.TemporaryDirectory() as directory:
      trace_path = Path(directory) / "addr_trace.txt"
      trace_path.write_text(
        "node_idx,access_idx,addr,size,is_store,base\n"
        "10,0,100,4,0,a\n"
        "11,1,104,4,0,a\n"
        "12,2,108,4,0,a\n"
        "13,3,112,4,0,a\n",
        encoding="utf-8",
      )
      remap_addr_trace(trace_path, trace_path, mapping)
      self.assertEqual(
        trace_path.read_text(encoding="utf-8").splitlines(),
        [
          "node_idx,access_idx,addr,size,is_store,base",
          "3,0,100,16,0,a",
        ],
      )

  def test_iterator_manifest_validates_and_returns_trace_reindex(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      (root / "probe_PartPredDFG.xml").write_text(
        "<DFG count=\"1\"><Node idx=\"0\"><OP>ADD</OP>"
        "<Inputs></Inputs><Outputs></Outputs><RecParents></RecParents>"
        "</Node></DFG>\n",
        encoding="utf-8",
      )
      (root / "probe_ITER_manifest.json").write_text(
        json.dumps(
          {
            "schema_version": 1,
            "transform": "externalize_iterator_only",
            "status": "applied",
            "original_node_count": 2,
            "removed_node_count": 1,
            "final_node_count": 1,
            "removed_nodes": [1],
            "artifact_vars_pruned": ["loopstart", "loopend"],
            "index_map": [{"original_idx": 0, "final_idx": 0}],
          }
        ),
        encoding="utf-8",
      )
      reindex, artifact_vars = validate_iter_manifest(
        SimpleNamespace(dfg_gen_kernel=str(root), kernel="probe")
      )
      self.assertEqual(reindex, {0: 0})
      self.assertEqual(artifact_vars, ("loopend", "loopstart"))


if __name__ == "__main__":
  unittest.main()
