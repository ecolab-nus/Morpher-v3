import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class IntermediateLiveOutTests(unittest.TestCase):
  def test_repeated_scalar_report_overwrites_one_slot(self):
    compiler = shutil.which("g++")
    if compiler is None:
      self.skipTest("g++ is required for the instrumentation contract test")

    repo = Path(__file__).resolve().parents[1]
    instrumentation = (
        repo / "Morpher_DFG_Generator/src/instrumentation/instrumentation.cpp"
    )
    header_dir = instrumentation.parent

    with tempfile.TemporaryDirectory() as tmp:
      tmp_path = Path(tmp)
      source = tmp_path / "liveout_contract.cpp"
      binary = tmp_path / "liveout_contract"
      source.write_text(
          """
#include "instrumentation.h"

int main() {
  loopStart("contract");
  LiveOutReportIntermediateVar("result", 0x01020304u);
  LiveOutReportIntermediateVar("result", 0x0a0b0c0du);
  loopEnd("contract");
  return 0;
}
""",
          encoding="utf-8",
      )
      (tmp_path / "memtraces").mkdir()
      subprocess.run(
          [
              compiler,
              "-std=c++17",
              "-I",
              str(header_dir),
              str(source),
              str(instrumentation),
              "-o",
              str(binary),
          ],
          check=True,
          capture_output=True,
          text=True,
      )
      subprocess.run(
          [str(binary)],
          cwd=tmp_path,
          check=True,
          capture_output=True,
          text=True,
      )

      rows = (tmp_path / "memtraces/contract_trace_0.txt").read_text(
          encoding="utf-8"
      ).splitlines()
      self.assertEqual(rows[0], "var_name,offset,pre-run-data,post-run-data")
      self.assertEqual(
          rows[1:],
          [
              "result,0,0,13",
              "result,1,0,12",
              "result,2,0,11",
              "result,3,0,10",
          ],
      )


if __name__ == "__main__":
  unittest.main()
