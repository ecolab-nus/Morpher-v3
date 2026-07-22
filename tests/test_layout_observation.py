import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class LayoutObservationOffsetTests(unittest.TestCase):
  def test_offset_recovery_is_limited_to_materialized_outer_accesses(self):
    compiler = shutil.which("g++")
    if compiler is None:
      self.skipTest("g++ is required for the layout-observation contract test")

    repo = Path(__file__).resolve().parents[1]
    include_dir = repo / "Morpher_CGRA_Mapper/include"
    with tempfile.TemporaryDirectory() as tmp:
      source = Path(tmp) / "layout_observation_contract.cpp"
      binary = Path(tmp) / "layout_observation_contract"
      source.write_text(
          r'''
#include "morpher/arch/LayoutObservation.h"

int main() {
  using CGRAXMLCompile::layoutObservationGepOffset;
  int base = 1024;
  if (layoutObservationGepOffset("LOAD", 12, 1036, &base) != 12) return 1;
  if (layoutObservationGepOffset("OLOAD", -1, 1024, &base) != 0) return 2;
  if (layoutObservationGepOffset("VOSTORE", -1, 1040, &base) != 16) return 3;
  if (layoutObservationGepOffset("LOAD", -1, 1040, &base) != -1) return 4;
  if (layoutObservationGepOffset("OSTORE", -1, 1040, nullptr) != -1) return 5;
  return 0;
}
''',
          encoding="utf-8",
      )
      subprocess.run(
          [compiler, "-std=c++17", "-I", str(include_dir), str(source),
           "-o", str(binary)],
          check=True,
          capture_output=True,
          text=True,
      )
      subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
  unittest.main()
