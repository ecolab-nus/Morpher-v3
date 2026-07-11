#include "ArtifactSim/morpher/BoundaryContract.h"

#include "ArtifactSim/core/Ops.h"
#include "morpher/arch/json.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using json = nlohmann::json;

namespace ArtifactSim {

namespace {

bool supportedReduction(const std::string &op) {
  return op == "ADD" || op == "AND" || op == "OR" || op == "XOR";
}

uint32_t readTraceScalar(const TraceImage &trace, const std::string &name,
                         bool post) {
  auto variableIt = trace.variables.find(name);
  if (variableIt == trace.variables.end()) {
    throw std::runtime_error("boundary trace variable is missing: " + name);
  }
  const auto &bytes = post ? variableIt->second.post : variableIt->second.pre;
  if (bytes.size() != 4) {
    throw std::runtime_error(
        "boundary trace variable must contain exactly one i32 value: " +
        name + " (found " + std::to_string(bytes.size()) + " bytes)");
  }
  uint32_t value = 0;
  for (uint32_t offset = 0; offset < 4; ++offset) {
    auto byteIt = bytes.find(offset);
    if (byteIt == bytes.end()) {
      throw std::runtime_error(
          "boundary trace variable has non-contiguous i32 bytes: " + name);
    }
    value |= static_cast<uint32_t>(byteIt->second) << (8 * offset);
  }
  return value;
}

void recordComparison(BoundaryResult &result, const std::string &label,
                      uint32_t actual, uint32_t expected) {
  if (actual == expected) {
    ++result.matches;
    return;
  }
  ++result.mismatches;
  if (result.mismatches <= 16) {
    std::cerr << "Boundary mismatch " << label << " actual=" << actual
              << " expected=" << expected << "\n";
  }
}

} // namespace

BoundaryContract parseBoundaryContract(const std::string &path) {
  BoundaryContract contract;
  if (path.empty()) {
    return contract;
  }

  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("could not open SIMD manifest: " + path);
  }
  json manifest;
  input >> manifest;
  if (!manifest.contains("exit_reductions") ||
      !manifest["exit_reductions"].is_array()) {
    throw std::runtime_error(
        "SIMD manifest lacks an exit_reductions array: " + path);
  }
  const int lanes = manifest.value("lanes", 0);
  if (lanes <= 0 && !manifest["exit_reductions"].empty()) {
    throw std::runtime_error("SIMD manifest has an invalid lane count: " + path);
  }

  std::set<int> sources;
  for (const auto &entry : manifest["exit_reductions"]) {
    if (!entry.is_object() || !entry.contains("source_final_idx") ||
        !entry.contains("reduce_op") ||
        !entry.contains("accumulator_identity") ||
        !entry.contains("scalar_init_trace_var") ||
        !entry.contains("scalar_exit_trace_var") ||
        !entry.contains("lane_seed_trace_vars") ||
        !entry.contains("lane_trace_vars")) {
      throw std::runtime_error(
          "SIMD manifest has an incomplete exit reduction contract: " + path);
    }
    AccumulatorBoundary boundary;
    boundary.sourceNode = entry["source_final_idx"].get<int>();
    boundary.lanes = lanes;
    boundary.reduceOp = entry["reduce_op"].get<std::string>();
    const std::string identity =
        entry["accumulator_identity"].get<std::string>();
    boundary.recurrenceIdentity = identity == "all_ones" ? UINT32_MAX : 0u;
    boundary.scalarInitTraceVar =
        entry["scalar_init_trace_var"].get<std::string>();
    boundary.scalarExitTraceVar =
        entry["scalar_exit_trace_var"].get<std::string>();
    boundary.laneSeedTraceVars =
        entry["lane_seed_trace_vars"].get<std::vector<std::string>>();
    boundary.laneTraceVars =
        entry["lane_trace_vars"].get<std::vector<std::string>>();

    const std::string expectedIdentity =
        boundary.reduceOp == "AND" ? "all_ones" : "zero";
    if (!supportedReduction(boundary.reduceOp) ||
        identity != expectedIdentity || boundary.sourceNode < 0 ||
        !sources.insert(boundary.sourceNode).second ||
        boundary.scalarInitTraceVar.empty() ||
        boundary.scalarExitTraceVar.empty() ||
        static_cast<int>(boundary.laneSeedTraceVars.size()) != lanes ||
        static_cast<int>(boundary.laneTraceVars.size()) != lanes ||
        entry.value("execution", std::string{}) != "loop_exit" ||
        entry.value("scheduled", true) ||
        entry.value("accumulator_init_execution", std::string{}) !=
            "loop_preheader" ||
        entry.value("accumulator_init_scheduled", true)) {
      throw std::runtime_error(
          "SIMD manifest has an inconsistent exit reduction contract: " +
          path);
    }
    std::set<std::string> laneVars;
    for (const std::string &name : boundary.laneSeedTraceVars) {
      if (name.empty() || !laneVars.insert(name).second) {
        throw std::runtime_error(
            "SIMD manifest has invalid accumulator lane seed trace variables: " +
            path);
      }
    }
    laneVars.clear();
    for (const std::string &name : boundary.laneTraceVars) {
      if (name.empty() || !laneVars.insert(name).second) {
        throw std::runtime_error(
            "SIMD manifest has invalid accumulator lane trace variables: " +
            path);
      }
    }
    contract.accumulators_.push_back(std::move(boundary));
  }
  return contract;
}

std::map<int, Value> BoundaryContract::recurrenceSeeds(
    const TraceImage &trace) const {
  std::map<int, Value> seeds;
  for (const AccumulatorBoundary &boundary : accumulators_) {
    std::vector<uint32_t> lanes;
    lanes.reserve(static_cast<size_t>(boundary.lanes));
    for (const std::string &name : boundary.laneSeedTraceVars) {
      const uint32_t seed = readTraceScalar(trace, name, false);
      if (seed != boundary.recurrenceIdentity) {
        throw std::runtime_error(
            "traced accumulator lane seed disagrees with manifest identity: " +
            name);
      }
      lanes.push_back(seed);
    }
    seeds.emplace(boundary.sourceNode, Value(std::move(lanes)));
  }
  return seeds;
}

BoundaryResult validateBoundaryContract(
    const BoundaryContract &contract,
    const std::map<int, Value> &finalNodeValues,
    const TraceImage &trace) {
  BoundaryResult result;
  for (const AccumulatorBoundary &boundary : contract.accumulators()) {
    auto valueIt = finalNodeValues.find(boundary.sourceNode);
    if (valueIt == finalNodeValues.end() || !valueIt->second.isValid() ||
        !valueIt->second.isVector() ||
        static_cast<int>(valueIt->second.lanes()) != boundary.lanes) {
      throw std::runtime_error(
          "mapped accumulator node " + std::to_string(boundary.sourceNode) +
          " did not produce its final " + std::to_string(boundary.lanes) +
          "-lane value");
    }

    const std::vector<uint32_t> &lanes = valueIt->second.vector();
    for (int lane = 0; lane < boundary.lanes; ++lane) {
      const uint32_t expected = readTraceScalar(
          trace, boundary.laneTraceVars[static_cast<size_t>(lane)], true);
      recordComparison(
          result,
          "node=" + std::to_string(boundary.sourceNode) +
              " lane=" + std::to_string(lane),
          lanes[static_cast<size_t>(lane)], expected);
    }

    const uint32_t reduced = reduceLanes(lanes, boundary.reduceOp);
    const uint32_t scalarInit =
        readTraceScalar(trace, boundary.scalarInitTraceVar, false);
    const uint32_t expectedExit =
        readTraceScalar(trace, boundary.scalarExitTraceVar, true);
    const uint32_t modeledExit =
        binaryOp(Value(scalarInit), Value(reduced), boundary.reduceOp).scalar();
    recordComparison(result,
                     "node=" + std::to_string(boundary.sourceNode) +
                         " host_exit",
                     modeledExit, expectedExit);
  }
  return result;
}

} // namespace ArtifactSim
