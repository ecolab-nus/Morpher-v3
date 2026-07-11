#pragma once

#include "ArtifactSim/core/Value.h"
#include "ArtifactSim/morpher/Trace.h"

#include <cstdint>
#include <map>
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

// Describes work intentionally omitted from the steady-state mapped DFG.
// The simulator seeds the mapped recurrence and checks its final lanes plus
// the host-executed scalar combine against named values from the source trace.
class BoundaryContract {
public:
  bool empty() const { return accumulators_.empty(); }
  const std::vector<AccumulatorBoundary> &accumulators() const {
    return accumulators_;
  }
  std::map<int, Value> recurrenceSeeds(const TraceImage &trace) const;

private:
  friend BoundaryContract parseBoundaryContract(const std::string &path);
  std::vector<AccumulatorBoundary> accumulators_;
};

struct BoundaryResult {
  int matches = 0;
  int mismatches = 0;
};

BoundaryContract parseBoundaryContract(const std::string &path);
BoundaryResult validateBoundaryContract(
    const BoundaryContract &contract,
    const std::map<int, Value> &finalNodeValues,
    const TraceImage &trace);

} // namespace ArtifactSim
