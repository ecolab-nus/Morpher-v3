import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from morpher_runner.context import RunnerConfig
from morpher_runner.dfg_generator import build_dfg_pass_command
from morpher_runner.dfg_generator import build_loop_interchange_command
from morpher_runner.dfg_generator import build_unroll_command
from morpher_runner.dfg_generator import validate_iter_manifest
from morpher_runner.dfg_generator import validate_simd_manifest
from morpher_runner.artifact_simulator import resolve_simd_manifest
from morpher_runner.mapper import build_mapper_commands
from morpher_runner.simd_packer import remap_addr_trace
from morpher_runner.simd_packer import SimdMapping
from run_morpher import _final_graph_recurrence_lower_bound
from run_morpher import _mapping_beam_width
from run_morpher import _candidate_can_improve


REPO_ROOT = Path(__file__).resolve().parents[2]
ARTIFACT_SIMULATOR = (
  REPO_ROOT / "simulator" / "artifact_simulator" / "build" / "artifact_simulator"
)


class ExternalizeAccessConfigTests(unittest.TestCase):

  def test_agi_pipeline_is_accepted(self):
    self.assertEqual(
      RunnerConfig._parse_externalize_access(
        {"dfg_pipeline": {"externalize_access": "agi"}}
      ),
      "agi",
    )

  def test_legacy_remove_agi_is_rejected(self):
    with self.assertRaisesRegex(RuntimeError, "were removed"):
      RunnerConfig._parse_externalize_access({"remove_agi": True})

  def test_agi_iter_is_accepted_for_the_restricted_control_only_mode(self):
    self.assertEqual(
      RunnerConfig._parse_externalize_access(
        {"dfg_pipeline": {"externalize_access": "agi_iter"}}
      ),
      "agi_iter",
    )

  def test_pipeline_does_not_combine_with_legacy_iterator_pruning(self):
    with self.assertRaisesRegex(RuntimeError, "were removed"):
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

  def test_verified_simd_accepts_iterator_externalization(self):
    self.assertEqual(
      RunnerConfig._parse_simd_pack(
        {"dfg_pipeline": {"simd_pack": "verified"}},
        "agi_iter",
      ),
      "verified",
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
      str(REPO_ROOT / "config" / "default.yaml")
    )
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
    )
    command = build_dfg_pass_command(context)
    self.assertIn("--externalize-access=agi_iter", command)

  def test_loop_interchange_command_uses_target_scoped_marker_pipeline(self):
    context = SimpleNamespace(
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
      dfg_gen_kernel="/tmp/probe/",
    )
    command = build_loop_interchange_command(context)
    self.assertIn(
      "-passes=morpher-loop-interchange",
      command.argv,
    )
    self.assertIn("probe_opt.ll", command.argv)
    self.assertIn("probe_li_opt.ll", command.argv)

  def test_auto_unroll_parses_loop_interchange_search(self):
    parsed = RunnerConfig._parse_auto_unroll(
      {"auto_unroll": {"enabled": True, "try_loop_interchange": True}}
    )
    self.assertTrue(parsed[3])

  def test_internal_unroll_defaults_and_explicit_factor_override(self):
    defaults = RunnerConfig._parse_auto_unroll({"auto_unroll": {"enabled": True}})
    explicit = RunnerConfig._parse_auto_unroll(
      {"auto_unroll": {"enabled": True, "factors": [3, 6]}}
    )
    self.assertEqual(defaults[1], [1, 2, 4, 8])
    self.assertEqual(explicit[1], [3, 6])
    self.assertTrue(defaults[3])
    self.assertFalse(defaults[4])

  def test_final_graph_recurrence_bound_ignores_stale_asap_levels(self):
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "probe.xml"
      path.write_text(
        '<DFG count="3">'
        '<Node idx="0" ASAP="40"><OP>ADD</OP><Outputs>'
        '<Output idx="1" nextiter="0"/></Outputs></Node>'
        '<Node idx="1" ASAP="70"><OP>ADD</OP><Outputs>'
        '<Output idx="2" nextiter="1"/></Outputs></Node>'
        '<Node idx="2" ASAP="2"><OP>CMERGE</OP><Outputs>'
        '<Output idx="0" nextiter="0"/></Outputs></Node>'
        '</DFG>\n',
        encoding="utf-8",
      )
      self.assertEqual(_final_graph_recurrence_lower_bound(path), 2)

  def test_final_graph_recurrence_bound_includes_reversed_recparents(self):
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "probe.xml"
      path.write_text(
        '<DFG count="3">'
        '<Node idx="0"><OP>LOAD</OP><Outputs>'
        '<Output idx="1" nextiter="0"/></Outputs><RecParents/></Node>'
        '<Node idx="1"><OP>ADD</OP><Outputs>'
        '<Output idx="2" nextiter="0"/></Outputs><RecParents/></Node>'
        '<Node idx="2"><OP>STORE</OP><Outputs/><RecParents>'
        '<RecParent idx="0"/></RecParents></Node>'
        '</DFG>\n',
        encoding="utf-8",
      )
      self.assertEqual(_final_graph_recurrence_lower_bound(path), 2)

  def test_mapping_beam_width_follows_preset(self):
    self.assertEqual(_mapping_beam_width("fast", 7), 1)
    self.assertEqual(_mapping_beam_width("trusted_pf", 7), 2)
    self.assertEqual(_mapping_beam_width("quality", 7), 4)
    self.assertEqual(_mapping_beam_width("exhaust", 7), 7)

  def test_adaptive_candidate_must_have_a_path_to_win(self):
    incumbent = {"score": 0.25, "factor": 8}
    self.assertFalse(_candidate_can_improve(
      {"lower_score": 0.25, "factor": 8}, incumbent
    ))
    self.assertFalse(_candidate_can_improve(
      {"lower_score": 0.25, "factor": 4}, incumbent
    ))
    self.assertTrue(_candidate_can_improve(
      {"lower_score": 0.125, "factor": 4}, incumbent
    ))

  def test_equal_bound_can_improve_factor_tie_breaker(self):
    incumbent = {"score": 0.25, "factor": 4}
    self.assertTrue(_candidate_can_improve(
      {"lower_score": 0.25, "factor": 8}, incumbent
    ))

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
    guard_index = mapper_command.index("--pf_guard_mode")
    self.assertEqual(mapper_command[guard_index + 1], "balanced")

  def test_fast_mapper_preset_selects_fast_progress_guard(self):
    config = RunnerConfig.from_yaml(
      str(REPO_ROOT / "config" / "simd4_verified_agi_iter.yaml")
    )
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      morpher_home=str(REPO_ROOT),
      mapper_home=str(REPO_ROOT / "Morpher_CGRA_Mapper"),
      mapper_kernel="/tmp",
      selected_simd_artifact_only=False,
    )
    commands, _ = build_mapper_commands(context)
    mapper_command = commands[-1].argv
    guard_index = mapper_command.index("--pf_guard_mode")
    self.assertEqual(mapper_command[guard_index + 1], "fast")

  def test_verified_simd_adds_cxx_pass_flags(self):
    config = RunnerConfig.from_yaml(
      str(REPO_ROOT / "config" / "simd4_verified_agi_iter.yaml")
    )
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
      selected_simd_enabled=True,
    )
    command = build_dfg_pass_command(context)
    self.assertIn("--simd-pack=4", command)
    self.assertTrue(any(arg.startswith("--simd-vector-ops=") for arg in command))
    self.assertIn("--simd-scalar-broadcast=1", command)

  def test_vred_arch_uses_exit_only_vector_partial_sum_lowering(self):
    config = RunnerConfig.from_yaml(
      str(REPO_ROOT / "config" / "simd4_verified_vred.yaml")
    )
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
      dfg_gen_kernel="/tmp/probe",
    )
    command = build_unroll_command(context, 4)
    policy_index = command.argv.index("--morpher-reduction-policy")
    self.assertEqual(command.argv[policy_index + 1], "vector-psum")

  def test_scalar_unroll_promotes_memory_reductions_without_claiming_simd_lanes(self):
    config = RunnerConfig.from_yaml(str(REPO_ROOT / "config" / "default.yaml"))
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
      dfg_gen_kernel="/tmp/probe",
    )
    command = build_unroll_command(context, 8)
    self.assertIn("--morpher-promote-memory-reductions", command.argv)
    lanes_index = command.argv.index("--morpher-reduction-lanes")
    self.assertEqual(command.argv[lanes_index + 1], "1")

  def test_vred_rejects_partial_vector_chunk_unroll(self):
    config = RunnerConfig.from_yaml(
      str(REPO_ROOT / "config" / "simd4_verified_vred.yaml")
    )
    context = SimpleNamespace(
      config=config,
      kernel="probe",
      dfg_gen_home=str(REPO_ROOT / "Morpher_DFG_Generator"),
      dfg_gen_kernel="/tmp/probe",
    )
    with self.assertRaisesRegex(RuntimeError, "partial vector chunks"):
      build_unroll_command(context, 6)

  def test_artifact_simulator_resolves_loop_named_simd_manifest(self):
    with tempfile.TemporaryDirectory() as directory:
      manifest = Path(directory) / "probe_INNERMOST_LN7_SIMD_manifest.json"
      manifest.write_text("{}\n", encoding="utf-8")
      context = SimpleNamespace(mapper_kernel=directory)
      self.assertEqual(resolve_simd_manifest(context), str(manifest))

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

  def test_iterator_manifest_accepts_post_simd_xml_index_space(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      (root / "probe_PartPredDFG.xml").write_text(
        "<DFG count=\"1\"><Node idx=\"0\"><OP>VADD</OP>"
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
            "original_node_count": 3,
            "removed_node_count": 1,
            "final_node_count": 2,
            "removed_nodes": [2],
            "artifact_vars_pruned": ["loopstart", "loopend"],
            "index_map": [
              {"original_idx": 0, "final_idx": 0},
              {"original_idx": 1, "final_idx": 1},
            ],
          }
        ),
        encoding="utf-8",
      )
      reindex, _ = validate_iter_manifest(
        SimpleNamespace(
          dfg_gen_kernel=str(root),
          kernel="probe",
          config=SimpleNamespace(simd_pack="verified"),
          selected_simd_enabled=True,
        )
      )
      self.assertEqual(reindex, {0: 0, 1: 1})

  def test_iterator_manifest_accepts_conservative_identity_skip(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      (root / "probe_PartPredDFG.xml").write_text(
        "<DFG count=\"2\"><Node idx=\"0\"><OP>MUL</OP></Node>"
        "<Node idx=\"1\"><OP>ADD</OP></Node></DFG>\n",
        encoding="utf-8",
      )
      (root / "probe_ITER_manifest.json").write_text(
        json.dumps(
          {
            "schema_version": 1,
            "transform": "externalize_iterator_only",
            "status": "skipped",
            "reason": "iterator control reaches a live accumulator",
            "original_node_count": 2,
            "removed_node_count": 0,
            "final_node_count": 2,
            "removed_nodes": [],
            "artifact_vars_pruned": [],
            "index_map": [
              {"original_idx": 0, "final_idx": 0},
              {"original_idx": 1, "final_idx": 1},
            ],
          }
        ),
        encoding="utf-8",
      )
      reindex, artifact_vars = validate_iter_manifest(
        SimpleNamespace(dfg_gen_kernel=str(root), kernel="probe")
      )
      self.assertEqual(reindex, {0: 0, 1: 1})
      self.assertEqual(artifact_vars, ())

  def test_vred_manifest_covers_pruned_and_synthesized_nodes(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      (root / "probe_PartPredDFG.xml").write_text(
        "<DFG count=\"2\">"
        "<Node idx=\"0\"><OP>VMUL</OP></Node>"
        "<Node idx=\"1\"><OP>VRED</OP></Node>"
        "</DFG>\n",
        encoding="utf-8",
      )
      (root / "probe_SIMD_manifest.json").write_text(
        json.dumps(
          {
            "schema_version": 1,
            "transform": "vector_pack",
            "status": "applied",
            "lanes": 4,
            "original_node_count": 5,
            "final_node_count": 2,
            "pruned_nodes": [4],
            "pruned_artifact_vars": ["morpher.reduce.tmp"],
            "synthesized_nodes": [
              {"final_idx": 1, "op": "VRED", "reduce_op": "ADD"}
            ],
            "packed_groups": [
              {
                "simd_group": 7,
                "final_idx": 0,
                "vector_op": "VMUL",
                "lanes": [0, 1, 2, 3],
              }
            ],
            "index_map": [
              {"original_idx": idx, "final_idx": 0} for idx in range(4)
            ],
          }
        ),
        encoding="utf-8",
      )
      mapping = validate_simd_manifest(
        SimpleNamespace(
          dfg_gen_kernel=str(root),
          kernel="probe",
          config=SimpleNamespace(arch_simd_width=4),
        )
      )
      self.assertEqual(mapping.simd_groups, {0: [0, 1, 2, 3]})
      self.assertEqual(mapping.artifact_vars_pruned, frozenset({"morpher.reduce.tmp"}))

  def test_vector_accumulator_manifest_requires_direct_self_recurrence(self):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      (root / "probe_PartPredDFG.xml").write_text(
        "<DFG count=\"1\">"
        "<Node idx=\"0\" simd_accumulator_op=\"ADD\" "
        "simd_accumulator_init_var=\"morpher.vpsum.init.9\" "
        "simd_accumulator_exit_var=\"morpher.vpsum.exit.9\" "
        "simd_accumulator_seed_var=\"morpher.vpsum.seed.9.0\"><OP>VADD</OP>"
        "<Inputs><Input idx=\"0\"/></Inputs>"
        "<Outputs><Output idx=\"0\" nextiter=\"1\" type=\"I1\"/></Outputs>"
        "<RecParents></RecParents></Node></DFG>\n",
        encoding="utf-8",
      )
      (root / "probe_SIMD_manifest.json").write_text(
        json.dumps(
          {
            "schema_version": 1,
            "transform": "vector_pack",
            "status": "applied",
            "lanes": 4,
            "original_node_count": 4,
            "final_node_count": 1,
            "post_pack_iterator_externalized": True,
            "pruned_nodes": [],
            "pruned_artifact_vars": ["morpher.vpsum.lane0", "dead.tmp"],
            "synthesized_nodes": [],
            "exit_reductions": [
              {
                "source_final_idx": 0,
                "op": "VRED",
                "reduce_op": "ADD",
                "accumulator_identity": "zero",
                "accumulator_init_execution": "loop_preheader",
                "accumulator_init_scheduled": False,
                "scalar_init_trace_var": "morpher.vpsum.init.9",
                "execution": "loop_exit",
                "scalar_exit_trace_var": "morpher.vpsum.exit.9",
                "combines_scalar_initializer": True,
                "stores_result": True,
                "scheduled": False,
                "lane_seed_trace_vars": [
                  "morpher.vpsum.seed.9.0",
                  "morpher.vpsum.seed.9.1",
                  "morpher.vpsum.seed.9.2",
                  "morpher.vpsum.seed.9.3",
                ],
                "lane_trace_vars": [
                  "morpher.vpsum.lane0",
                  "morpher.vpsum.lane1",
                  "morpher.vpsum.lane2",
                  "morpher.vpsum.lane3",
                ],
              }
            ],
            "packed_groups": [
              {
                "simd_group": 9,
                "final_idx": 0,
                "vector_op": "VADD",
                "lanes": [0, 1, 2, 3],
              }
            ],
            "index_map": [
              {"original_idx": idx, "final_idx": 0} for idx in range(4)
            ],
          }
        ),
        encoding="utf-8",
      )
      mapping = validate_simd_manifest(
        SimpleNamespace(
          dfg_gen_kernel=str(root),
          kernel="probe",
          config=SimpleNamespace(arch_simd_width=4),
        )
      )
      self.assertTrue(mapping.post_pack_iterator_externalized)
      self.assertEqual(mapping.artifact_vars_pruned, frozenset({"dead.tmp"}))
      self.assertEqual(mapping.host_modeled_boundaries, 1)
      self.assertEqual(
        mapping.boundary_trace_vars,
        frozenset(
          {
            "morpher.vpsum.init.9",
            "morpher.vpsum.exit.9",
            "morpher.vpsum.seed.9.0",
            "morpher.vpsum.seed.9.1",
            "morpher.vpsum.seed.9.2",
            "morpher.vpsum.seed.9.3",
            "morpher.vpsum.lane0",
            "morpher.vpsum.lane1",
            "morpher.vpsum.lane2",
            "morpher.vpsum.lane3",
          }
        ),
      )


@unittest.skipUnless(ARTIFACT_SIMULATOR.is_file(), "artifact_simulator is not built")
class BoundaryContractSimulatorTests(unittest.TestCase):

  def run_fixture(self, lane2_expected=10, seed2=0):
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      (root / "dfg.xml").write_text(
        '<DFG count="1"><Node idx="0" ASAP="0" ALAP="0" CONST="5" '
        'simd_packed="true" simd_lanes="4"><OP>VADD</OP>'
        '<Inputs><Input idx="0"/></Inputs><Outputs>'
        '<Output idx="0" nextiter="1" type="I1"/></Outputs>'
        '<RecParents></RecParents></Node></DFG>\n',
        encoding="utf-8",
      )
      (root / "mapping.txt").write_text("functional placeholder\n", encoding="utf-8")
      (root / "mem_alloc.txt").write_text("var_name,address\n", encoding="utf-8")
      values = {
        "init": (7, 7),
        "exit": (0, 47),
        "seed0": (0, 0),
        "seed1": (0, 0),
        "seed2": (seed2, seed2),
        "seed3": (0, 0),
        "lane0": (0, 10),
        "lane1": (0, 10),
        "lane2": (0, lane2_expected),
        "lane3": (0, 10),
      }
      trace_lines = ["var_name,offset,pre-run-data,post-run-data"]
      for name, (pre, post) in values.items():
        for offset in range(4):
          trace_lines.append(
            "%s,%d,%d,%d"
            % (name, offset, (pre >> (8 * offset)) & 0xff,
               (post >> (8 * offset)) & 0xff)
          )
      (root / "trace.txt").write_text(
        "\n".join(trace_lines) + "\n", encoding="utf-8"
      )
      (root / "manifest.json").write_text(
        json.dumps(
          {
            "lanes": 4,
            "exit_reductions": [
              {
                "source_final_idx": 0,
                "reduce_op": "ADD",
                "accumulator_identity": "zero",
                "accumulator_init_execution": "loop_preheader",
                "accumulator_init_scheduled": False,
                "scalar_init_trace_var": "init",
                "execution": "loop_exit",
                "scalar_exit_trace_var": "exit",
                "scheduled": False,
                "lane_seed_trace_vars": ["seed0", "seed1", "seed2", "seed3"],
                "lane_trace_vars": ["lane0", "lane1", "lane2", "lane3"],
              }
            ],
          }
        ),
        encoding="utf-8",
      )
      return subprocess.run(
        [
          str(ARTIFACT_SIMULATOR),
          "--dfg", str(root / "dfg.xml"),
          "--mapping", str(root / "mapping.txt"),
          "--mem-alloc", str(root / "mem_alloc.txt"),
          "--trace", str(root / "trace.txt"),
          "--simd-manifest", str(root / "manifest.json"),
          "--max-iters", "2",
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
      )

  def test_trace_backed_boundary_contract_passes(self):
    result = self.run_fixture()
    self.assertEqual(result.returncode, 0, result.stdout)
    self.assertIn("Boundary Validation: Matches::5, Mismatches::0", result.stdout)

  def test_wrong_final_lane_cannot_pass_vacuously(self):
    result = self.run_fixture(lane2_expected=11)
    self.assertEqual(result.returncode, 2, result.stdout)
    self.assertIn("Boundary Validation: Matches::4, Mismatches::1", result.stdout)

  def test_traced_seed_must_match_recurrence_identity(self):
    result = self.run_fixture(seed2=1)
    self.assertEqual(result.returncode, 1, result.stdout)
    self.assertIn("lane seed disagrees with manifest identity", result.stdout)


if __name__ == "__main__":
  unittest.main()
