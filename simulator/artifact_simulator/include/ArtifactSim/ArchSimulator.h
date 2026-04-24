#pragma once

#include "ArtifactSim/ArchSchedule.h"
#include "ArtifactSim/CGRAMesh.h"
#include "ArtifactSim/SimulatorBase.h"

namespace ArtifactSim {

// Arch-aware simulator.
// Executes the mapper's schedule in latency order, using the CGRA mesh to
// validate that every DFG edge can be routed within its cycle budget.
// Execution logic is inherited from SimulatorBase; the arch model adds:
//   - Routing validation at construction time (ArchSchedule::build).
//   - PE capability awareness (warns on unsupported ops).
class ArchSimulator : public SimulatorBase {
public:
    ArchSimulator(CGRAArch arch, ArchSchedule schedule,
                  DFGGraph graph, Mapping mapping,
                  MemAlloc alloc, TraceImage trace, SimOptions options);

    SimResult run();

private:
    CGRAMesh mesh_;
    ArchSchedule schedule_;
};

} // namespace ArtifactSim
