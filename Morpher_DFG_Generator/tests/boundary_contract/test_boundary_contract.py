import json
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


GENERATOR_ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).with_name("boundary_contract.c")
PLUGIN = GENERATOR_ROOT / "build" / "src" / "libdfggenPass.so"
CLANG = shutil.which("clang-18") or shutil.which("clang")
OPT = shutil.which("opt-18") or shutil.which("opt")


@unittest.skipUnless(PLUGIN.is_file() and CLANG and OPT, "built LLVM 18 generator is required")
class BoundaryCanonicalizationRegressions(unittest.TestCase):

  @classmethod
  def setUpClass(cls):
    cls._temp = tempfile.TemporaryDirectory()
    cls.root = Path(cls._temp.name)
    source_ir = cls.root / "boundary_contract.ll"
    cls.optimized_ir = cls.root / "boundary_contract_opt.ll"
    subprocess.run(
      [
        CLANG,
        "-target", "i386-unknown-linux-gnu",
        "-c", "-emit-llvm", "-O2", "-ffp-contract=off", "-fno-inline",
        "-fno-vectorize", "-fno-slp-vectorize", "-fno-unroll-loops",
        str(FIXTURE), "-S", "-o", str(source_ir),
      ],
      check=True,
      text=True,
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
    )
    subprocess.run(
      [OPT, "-passes=default<O2>", str(source_ir), "-S", "-o", str(cls.optimized_ir)],
      check=True,
      text=True,
      stdout=subprocess.PIPE,
      stderr=subprocess.PIPE,
    )

  @classmethod
  def tearDownClass(cls):
    cls._temp.cleanup()

  def generate(self, function, externalize_iterator=True):
    work = self.root / function
    work.mkdir()
    output_ir = work / "instrumented.ll"
    command = [
        OPT,
        "-load-pass-plugin", str(PLUGIN),
        "-passes=skeleton",
        "--fn", function,
        "--nobanks", "4",
        "--banksize", "65536",
        "--canonicalize-loop-boundaries",
        "--native-stateful-mac",
        "--treg-reset-on-launch",
        "--suppress-dfg-dot",
        "-S", str(self.optimized_ir), "-o", str(output_ir),
      ]
    if externalize_iterator:
      command.insert(command.index("--canonicalize-loop-boundaries"), "--externalize-access=agi_iter")
    result = subprocess.run(
      command,
      cwd=work,
      check=False,
      text=True,
      stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT,
    )
    self.assertEqual(result.returncode, 0, result.stdout)
    xml_path = work / (function + "_PartPredDFG.xml")
    self.assertTrue(xml_path.is_file(), result.stdout)
    wrapped = ET.fromstring(
      "<Root>\n%s\n</Root>" % xml_path.read_text(encoding="utf-8")
    )
    graph = wrapped.find("DFG")
    manifests = list(work.glob("*_BOUNDARY_manifest.json"))
    manifest = json.loads(manifests[0].read_text(encoding="utf-8")) if manifests else None
    return graph, manifest

  def test_zero_seed_fuses_to_reset_backed_mac(self):
    graph, manifest = self.generate("zero_seeded_mac")
    self.assertIn("MAC", [node.findtext("OP") for node in graph.findall("Node")])
    self.assertEqual(manifest["schema_version"], 2)
    self.assertEqual(
      manifest["launch"],
      {
        "execution": "before_each_loop_invocation",
        "actions": [
          {"operation": "reset", "storage": "treg", "scope": "all_pes", "value": 0}
        ],
      },
    )

  def test_nonzero_seed_uses_explicit_recurrence(self):
    graph, manifest = self.generate("nonzero_seeded_mac")
    state = manifest["states"][0]
    self.assertEqual(state["storage"], "explicit_recurrence")
    self.assertEqual(state["seed"]["value"], 7)
    self.assertEqual(manifest["launch"]["actions"], [])
    state_nodes = [
      candidate for candidate in graph.findall("Node")
      if candidate.get("loop_carried_state") == "true"
    ]
    self.assertEqual(
      len(state_nodes), 1,
      "\n".join(ET.tostring(candidate, encoding="unicode") for candidate in graph.findall("Node")),
    )
    node = state_nodes[0]
    self.assertEqual(int(node.get("idx")), state["mapped_node"])
    self.assertTrue(any(
      edge.get("idx") == node.get("idx") and edge.get("nextiter") == "1"
      for edge in node.findall("./Outputs/Output")
    ), ET.tostring(node, encoding="unicode"))

  def test_nonconstant_seed_leaves_graph_unchanged(self):
    graph, manifest = self.generate("nonconstant_seeded_mac")
    self.assertIsNone(manifest)
    ops = [node.findtext("OP") for node in graph.findall("Node")]
    self.assertIn("SELECT", ops)
    self.assertIn("CMERGE", ops)

  def test_genuine_predicated_select_is_preserved(self):
    graph, _ = self.generate("predicated_select", externalize_iterator=False)
    ops = [node.findtext("OP") for node in graph.findall("Node")]
    self.assertTrue("SELECT" in ops or "CMERGE" in ops)


if __name__ == "__main__":
  unittest.main()
