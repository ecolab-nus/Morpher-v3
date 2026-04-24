#pragma once

#include "ArtifactSim/SimulatorBase.h"

namespace ArtifactSim {

class Simulator : public SimulatorBase {
public:
  Simulator(DFGGraph graph, Mapping mapping, MemAlloc alloc, TraceImage trace, SimOptions options);
  SimResult run();
};

} // namespace ArtifactSim
