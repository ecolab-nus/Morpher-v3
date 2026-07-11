#pragma once

#include "ArtifactSim/morpher/BoundaryContract.h"
#include "ArtifactSim/morpher/DFG.h"
#include "ArtifactSim/morpher/Memory.h"
#include "ArtifactSim/morpher/NodeSemantics.h"
#include "ArtifactSim/morpher/Trace.h"

#include <map>

namespace ArtifactSim {

// Arch-unaware golden checker: replays the DFG in dependency order
// (ASAP buckets, Kahn within a bucket), one full pass per loop iteration,
// then compares the final memory image against the expected trace.
// Fast, ignores the fabric entirely; use it to separate "the mapping is
// wrong" from "the DFG/lowering is wrong".
class FunctionalSimulator {
public:
  FunctionalSimulator(DFGGraph graph, MemAlloc alloc, TraceImage trace,
                      int maxIterations,
                      BoundaryContract boundary = BoundaryContract{});

  SimResult run();

private:
  DFGGraph graph_;
  BoundaryContract boundary_;
  NodeExecutor exec_;
  int maxIterations_;
  std::map<int, Value> values_;
  std::map<int, Value> previousValues_;
};

} // namespace ArtifactSim
