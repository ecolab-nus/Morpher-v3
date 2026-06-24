#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ArtifactSim/DFG.h"
#include "ArtifactSim/Mapping.h"
#include "ArtifactSim/Memory.h"
#include "ArtifactSim/Trace.h"
#include "ArtifactSim/Value.h"

namespace ArtifactSim {

struct SimOptions {
  int xdim = 4;
  int ydim = 4;
  int maxIterations = 1024;
};

struct SimResult {
  int matches = 0;
  int mismatches = 0;
};

// Shared simulation state and logic for ArtifactSimulator and ArchSimulator.
// Input resolution, operation dispatch, memory access, and result comparison
// are identical between the two simulators; only the run() loop and
// construction differ.
class SimulatorBase {
protected:
  SimulatorBase(DFGGraph graph, Mapping mapping, MemAlloc alloc,
                TraceImage trace, SimOptions options);

  Value inputValue(const DFGNode &node, size_t index) const;
  Value inputByType(const DFGNode &node, const std::string &type) const;
  Value firstDataInput(const DFGNode &node) const;
  Value secondDataInput(const DFGNode &node) const;
  bool predicateAllows(const DFGNode &node) const;
  std::vector<uint32_t> memoryAddresses(const DFGNode &node);
  Value executeNode(const DFGNode &node,
                    const Value *physicalMacAccumulator = nullptr);
  bool shouldStop() const;
  SimResult compare() const;

public:
  const TraceImage &traceImage() const;

protected:
  DFGGraph graph_;
  Mapping mapping_;
  MemAlloc alloc_;
  TraceImage trace_;
  MemoryImage initialSnapshot_;
  SimOptions options_;
  std::map<int, Value> values_;
  std::map<int, Value> previousValues_;
  std::map<std::string, uint32_t> implicitOffsets_;
  std::map<int, size_t> externalAddressOffsets_;
  std::map<int, size_t> externalValueOffsets_;
  std::set<uint32_t> writtenAddresses_;
};

} // namespace ArtifactSim
