#pragma once

#include <map>
#include <string>

#include "ArtifactSim/DFG.h"
#include "ArtifactSim/Mapping.h"
#include "ArtifactSim/Memory.h"
#include "ArtifactSim/Mesh.h"
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

class Simulator {
public:
  Simulator(DFGGraph graph, Mapping mapping, MemAlloc alloc, TraceImage trace, SimOptions options);
  SimResult run();

private:
  Value executeNode(const DFGNode &node);
  Value inputValue(const DFGNode &node, size_t index) const;
  Value firstDataInput(const DFGNode &node) const;
  Value secondDataInput(const DFGNode &node) const;
  Value inputByType(const DFGNode &node, const std::string &type) const;
  bool predicateAllows(const DFGNode &node) const;
  std::vector<uint32_t> memoryAddresses(const DFGNode &node);
  void storeNodeResult(const DFGNode &node, const Value &value);
  bool shouldStop() const;
  SimResult compare() const;

  DFGGraph graph_;
  Mapping mapping_;
  MemAlloc alloc_;
  TraceImage trace_;
  SimOptions options_;
  Mesh mesh_;
  std::map<int, Value> values_;
  std::map<int, Value> previousValues_;
  std::map<std::string, uint32_t> implicitOffsets_;
  std::map<int, size_t> externalAddressOffsets_;
};

} // namespace ArtifactSim
