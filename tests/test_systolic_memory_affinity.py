import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class SystolicMemoryAffinityTests(unittest.TestCase):
  def test_geometry_and_same_ii_fallback_contract(self):
    compiler = shutil.which("g++")
    if compiler is None:
      self.skipTest("g++ is required for the systolic-affinity contract test")

    repo = Path(__file__).resolve().parents[1]
    include_dir = repo / "Morpher_CGRA_Mapper/include"
    with tempfile.TemporaryDirectory() as tmp:
      source = Path(tmp) / "systolic_memory_affinity_contract.cpp"
      binary = Path(tmp) / "systolic_memory_affinity_contract"
      source.write_text(
          r'''
#include "morpher/mapper/SystolicMemoryAffinity.h"

int main() {
  using namespace CGRAXMLCompile;
  // Compute array spans X=[1,4], Y=[0,3].
  if (!matchesSystolicMemorySide("row", 0, 2, 1, 3)) return 1;
  if (matchesSystolicMemorySide("row", 5, 2, 1, 3)) return 2;
  if (!matchesSystolicMemorySide("column", 2, 4, 1, 3)) return 3;
  if (matchesSystolicMemorySide("column", 0, 2, 1, 3)) return 4;
  if (matchesSystolicMemorySide("unknown", 0, 2, 1, 3)) return 5;
  if (!matchesIndexedSystolicStream("row", 2, 0, 2, 1, 0, 3)) return 6;
  if (matchesIndexedSystolicStream("row", 2, 0, 1, 1, 0, 3)) return 7;
  if (!matchesIndexedSystolicStream("column", 3, 4, 4, 1, 0, 3)) return 8;
  if (matchesIndexedSystolicStream("column", 3, 3, 4, 1, 0, 3)) return 9;
  if (!matchesSystolicMacCoordinate(2, 3, 4, 2, 1, 0, 4, 3)) return 10;
  if (matchesSystolicMacCoordinate(2, 3, 3, 2, 1, 0, 4, 3)) return 11;
  if (matchesSystolicMacCoordinate(4, 0, 1, 4, 1, 0, 4, 3)) return 12;
  if (systolicPathFinderAttemptCount(1, true) != 2) return 13;
  if (systolicPathFinderAttemptCount(3, true) != 3) return 14;
  if (systolicPathFinderAttemptCount(1, false) != 1) return 15;
  if (!enforceSystolicSidesOnAttempt(true, false, 0)) return 16;
  if (enforceSystolicSidesOnAttempt(true, false, 1)) return 17;
  if (enforceSystolicSidesOnAttempt(true, true, 0)) return 18;
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
