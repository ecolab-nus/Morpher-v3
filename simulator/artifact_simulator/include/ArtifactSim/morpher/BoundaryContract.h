#pragma once

#include "ArtifactSim/core/Value.h"
#include "ArtifactSim/morpher/Trace.h"

#include <cstdint>
#include <map>
#include <utility>
#include <string>
#include <vector>

namespace ArtifactSim {

struct AccumulatorBoundary {
  int sourceNode = -1;
  int lanes = 0;
  std::string reduceOp;
  uint32_t recurrenceIdentity = 0;
  std::string scalarInitTraceVar;
  std::string scalarExitTraceVar;
  std::vector<std::string> laneSeedTraceVars;
  std::vector<std::string> laneTraceVars;
};

struct StateBoundary {
  int sourceNode = -1;
  int lanes = 1;
  int valueBits = 32;
  std::string storage;
  uint32_t seedValue = 0;
  std::vector<std::string> finalTraceVars;
};

struct StationaryPreloadBoundary {
  int sourceNode = -1;
  int operand = -1;
  int valueBits = 32;
  int row = -1;
  int column = -1;
  std::string traceVar;
  uint32_t byteOffset = 0;
};

// Describes work intentionally omitted from the steady-state mapped DFG.
// The simulator seeds the mapped recurrence and checks its final lanes plus
// the host-executed scalar combine against named values from the source trace.
class BoundaryContract {
public:
  bool empty() const {
    return accumulators_.empty() && states_.empty() && stationaryPreloads_.empty();
  }
  bool tregResetOnLaunch() const { return tregResetOnLaunch_; }
  const std::vector<AccumulatorBoundary> &accumulators() const {
    return accumulators_;
  }
  std::map<int, Value> recurrenceSeeds(const TraceImage &trace) const;
  const std::vector<StateBoundary> &states() const { return states_; }
  std::map<std::pair<int, int>, Value>
  stationaryOperands(const TraceImage &trace) const;
  const std::vector<StationaryPreloadBoundary> &stationaryPreloads() const {
    return stationaryPreloads_;
  }

private:
  friend BoundaryContract parseBoundaryContract(const std::string &path);
  std::vector<AccumulatorBoundary> accumulators_;
  std::vector<StateBoundary> states_;
  std::vector<StationaryPreloadBoundary> stationaryPreloads_;
  bool tregResetOnLaunch_ = false;
};

struct BoundaryResult {
  int matches = 0;
  int mismatches = 0;
};

BoundaryContract parseBoundaryContract(const std::string &path);
void applyLaunchStateActions(
    const BoundaryContract &contract, bool architectureSupportsTregReset,
    std::map<std::pair<int, int>, Value> &macState);
BoundaryResult validateBoundaryContract(
    const BoundaryContract &contract,
    const std::map<int, Value> &finalNodeValues,
    const TraceImage &trace);

} // namespace ArtifactSim
