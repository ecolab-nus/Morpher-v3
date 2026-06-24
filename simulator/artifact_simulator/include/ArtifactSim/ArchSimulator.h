#pragma once

#include "ArtifactSim/ArchSchedule.h"
#include "ArtifactSim/CGRAMesh.h"
#include "ArtifactSim/SimulatorBase.h"

#include <map>
#include <utility>

namespace ArtifactSim {

// Arch-aware simulator.
// Executes the mapper's schedule in latency order, using the CGRA mesh to
// validate that every DFG edge can be routed within its cycle budget.
// The arch model adds:
//   - Routing validation at construction time (ArchSchedule::build).
//   - PE capability awareness (warns on unsupported ops).
//   - One implicit MAC accumulator (TREG) per physical PE.
class ArchSimulator : public SimulatorBase {
public:
    ArchSimulator(CGRAArch arch, ArchSchedule schedule,
                  DFGGraph graph, Mapping mapping,
                  MemAlloc alloc, TraceImage trace, SimOptions options);

    SimResult run();
    SimResult runWithState(std::map<std::pair<int, int>, Value> &tregValues,
                           bool compareAfter);

private:
    CGRAMesh mesh_;
    ArchSchedule schedule_;
};

} // namespace ArtifactSim
