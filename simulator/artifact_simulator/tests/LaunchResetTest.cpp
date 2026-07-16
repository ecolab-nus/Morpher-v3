#include "ArtifactSim/morpher/BoundaryContract.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>

using namespace ArtifactSim;

int main() {
  const std::string path = "/tmp/artifact_sim_launch_reset_test.json";
  {
    std::ofstream manifest(path);
    manifest << R"({
      "schema_version": 2,
      "transform": "loop_boundary_canonicalization",
      "status": "applied",
      "launch": {
        "execution": "before_each_loop_invocation",
        "actions": [
          {"operation": "reset", "storage": "treg", "scope": "all_pes", "value": 0},
          {"operation": "preload", "storage": "regfile", "mapped_node": 7,
           "operand": "I2", "row": 1, "column": 2, "value_bits": 32,
           "trace_address": {"variable": "weight", "byte_offset": 4},
           "scheduled": false}
        ]
      },
      "states": [{
        "mapped_node": 0,
        "lanes": 1,
        "value_bits": 32,
        "storage": "treg",
        "seed": {"kind": "register_reset", "value": 0,
                 "execution": "loop_launch", "scheduled": false},
        "drain": {"execution": "loop_exit", "scheduled": false,
                  "trace_vars": ["out"]}
      }],
      "stationary_preloads": [{
        "mapped_node": 7, "operand": "I2", "value_bits": 32,
        "row": 1, "column": 2,
        "trace_address": {"variable": "weight", "byte_offset": 4}
      }]
    })";
  }

  try {
    const BoundaryContract contract = parseBoundaryContract(path);
    TraceImage trace;
    trace.variables["weight"].pre = {
      {4, 0x78}, {5, 0x56}, {6, 0x34}, {7, 0x12}
    };
    const auto stationary = contract.stationaryOperands(trace);
    if (stationary.at({7, 2}).scalar() != 0x12345678u) return 5;
    std::map<std::pair<int, int>, Value> state;

    state[{0, 0}] = Value(17);
    applyLaunchStateActions(contract, true, state);
    if (!state.empty()) return 1;

    // A second dynamic invocation must clear state produced by the first.
    state[{0, 0}] = Value(29);
    state[{1, 0}] = Value(31);
    applyLaunchStateActions(contract, true, state);
    if (!state.empty()) return 2;

    state[{0, 0}] = Value(1);
    try {
      applyLaunchStateActions(contract, false, state);
      return 3;
    } catch (const std::runtime_error &) {
      // Expected: an artifact cannot manufacture an absent architecture feature.
    }
  } catch (...) {
    std::remove(path.c_str());
    return 4;
  }

  std::remove(path.c_str());
  return 0;
}
