#pragma once

#include "ArtifactSim/core/ConfigMem.h"
#include "ArtifactSim/core/Engine.h"
#include "ArtifactSim/core/Fabric.h"
#include "ArtifactSim/morpher/DFG.h"
#include "ArtifactSim/morpher/Memory.h"
#include "ArtifactSim/morpher/NodeSemantics.h"
#include "ArtifactSim/morpher/Trace.h"

#include <map>
#include <utility>

namespace ArtifactSim {

// Glue between the generic cycle engine and Morpher node semantics:
// FU issues execute DFG nodes against the trace-driven memory model, with
// operands sampled from the FU's ports.
//
// MAC/VMAC accumulator state is a physical per-PE register (keyed by grid
// coordinates) that survives across phases of multi-phase kernels, mirroring
// hardware TREG behaviour.
class CycleSimulator {
public:
  CycleSimulator(const Fabric &fabric, const ConfigMem &config,
                 const DFGGraph &graph, MemAlloc alloc, TraceImage trace,
                 int maxIterations);

  SimResult run(std::map<std::pair<int, int>, Value> &macState,
                bool compareAfter);

  const TraceImage &traceImage() const { return exec_.traceImage(); }

private:
  const Fabric &fabric_;
  const ConfigMem &config_;
  const DFGGraph &graph_;
  NodeExecutor exec_;
  int maxIterations_;
};

} // namespace ArtifactSim
