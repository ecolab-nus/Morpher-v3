import unittest
from pathlib import Path

from scripts.bank_spread_report import matching_decision_path, summarize_observation


class BankSpreadReportTests(unittest.TestCase):
  def test_summary_deduplicates_time_steps_and_banks(self):
    doc = {
        "arrays": {
            "a": {
                "ld_st_node_idxs": [1, 2, 3],
                "memPEs_touched": [
                    {"x": 0, "y": 1, "t": 0, "bank": "B0"},
                    {"x": 0, "y": 1, "t": 1, "bank": "B0"},
                    {"x": 2, "y": 1, "t": 0, "bank": "B1"},
                ],
            },
            "b": {"ld_st_node_idxs": [4], "memPEs_touched": []},
        }
    }
    summary = summarize_observation(doc)
    self.assertEqual(summary["total_bank_spread"], 2)
    self.assertEqual(summary["rows"][0]["mem_pes"], 2)
    self.assertEqual(summary["rows"][0]["banks"], ["B0", "B1"])

  def test_matching_decision_filename(self):
    path = Path("kernel_layout_observation.json")
    self.assertEqual(
        matching_decision_path(path), Path("kernel_layout_decision.json")
    )


if __name__ == "__main__":
  unittest.main()
