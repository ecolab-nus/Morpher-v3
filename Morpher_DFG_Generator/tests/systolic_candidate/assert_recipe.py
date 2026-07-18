#!/usr/bin/env python3
"""Focused assertions for the versioned systolic transform recipe."""

import json
import sys
from pathlib import Path


def main() -> int:
  fixture = sys.argv[1]
  records = [
    json.loads(line)
    for line in Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()
    if line.strip()
  ]
  if fixture == "gemm_two_nests":
    assert len(records) == 2
    assert [record["candidate_index"] for record in records] == [0, 1]
    assert [record["contains_map_marker"] for record in records] == [True, False]
    for record in records:
      recipe = record["transform_recipe"]
      assert recipe["schema_version"] == 1
      assert recipe["transform_supported"] is True
      assert recipe["unsupported_reason"] is None
    return 0
  if len(records) != 1:
    raise AssertionError(f"{fixture}: expected one record, got {len(records)}")

  record = records[0]
  recipe = record["transform_recipe"]
  assert recipe["schema_version"] == 1
  assert recipe["candidate_id"] == f"{fixture}#0"
  assert record["contains_map_marker"] is fixture.endswith("_marker")

  if fixture == "gemm_ijk":
    assert recipe["transform_supported"] is True
    assert recipe["unsupported_reason"] is None
    assert [item["level"] for item in recipe["parallel_dimensions"]] == [0, 1]
    assert [item["trip_count"] for item in recipe["parallel_dimensions"]] == [16, 16]
    assert all(
      item["legal_factors"] == [2, 4, 8, 16]
      for item in recipe["parallel_dimensions"]
    )
    assert recipe["rolled_reduction"] == {"level": 2, "trip_count": 16}
  elif fixture == "gemm_ikj":
    assert recipe["transform_supported"] is False
    assert recipe["unsupported_reason"] == (
      "v1 requires the reduction loop to be innermost"
    )
    assert recipe["parallel_dimensions"] == []
    assert recipe["rolled_reduction"] is None
  elif fixture == "gemm_dynamic":
    assert recipe["transform_supported"] is False
    assert recipe["unsupported_reason"] == (
      "v1 requires constant positive trip counts"
    )
  elif fixture == "gemm_n10":
    assert recipe["transform_supported"] is True
    assert all(
      item["legal_factors"] == [2, 5, 10]
      for item in recipe["parallel_dimensions"]
    )
  elif fixture == "matvec":
    assert recipe["transform_supported"] is True
    assert recipe["unsupported_reason"] is None
    assert recipe["parallel_dimensions"] == [{
      "level": 0,
      "trip_count": 16,
      "requires_exact_divisor": True,
      "legal_factors": [2, 4, 8, 16],
    }]
    assert recipe["rolled_reduction"] == {"level": 1, "trip_count": 16}

  return 0


if __name__ == "__main__":
  raise SystemExit(main())
