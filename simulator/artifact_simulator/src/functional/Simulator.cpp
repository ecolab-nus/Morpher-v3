#include "ArtifactSim/functional/Simulator.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ArtifactSim {

namespace {

// Operands come from the per-iteration value maps, following DFG edges.
// nextIter edges read the previous iteration's map.
class MapOperandSource : public OperandSource {
public:
  MapOperandSource(const std::map<int, Value> &values,
                   const std::map<int, Value> &previousValues)
      : values_(values), previousValues_(previousValues) {}

  Value inputValue(const DFGNode &node, size_t index) const override {
    if (index >= node.inputs.size()) {
      if (node.hasConst) {
        return node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes)
                               : Value(node.constValue);
      }
      return Value::invalid();
    }
    const int inputIdx = node.inputs[index].idx;
    if (node.inputs[index].nextIter) {
      auto prevIt = previousValues_.find(inputIdx);
      if (prevIt != previousValues_.end()) {
        return prevIt->second;
      }
    } else {
      auto it = values_.find(inputIdx);
      if (it != values_.end()) {
        return it->second;
      }
    }
    return Value::invalid();
  }

  Value selfPreviousValue(const DFGNode &node) const override {
    auto prevIt = previousValues_.find(node.idx);
    return prevIt != previousValues_.end() ? prevIt->second : Value::invalid();
  }

private:
  const std::map<int, Value> &values_;
  const std::map<int, Value> &previousValues_;
};

} // namespace

FunctionalSimulator::FunctionalSimulator(DFGGraph graph, MemAlloc alloc,
                                         TraceImage trace, int maxIterations)
    : graph_(std::move(graph)),
      exec_(graph_, std::move(alloc), std::move(trace)),
      maxIterations_(maxIterations) {}

SimResult FunctionalSimulator::run() {
  const std::vector<int> ordered = canonicalOrder(graph_);
  const bool debug = std::getenv("ARTIFACT_SIM_DEBUG") != nullptr;

  for (int iter = 0; iter < maxIterations_; ++iter) {
    previousValues_ = values_;
    values_.clear();
    MapOperandSource operands(values_, previousValues_);
    for (int idx : ordered) {
      const DFGNode &node = graph_.node(idx);
      Value out = exec_.execute(node, operands);
      if (debug) {
        std::cerr << "iter=" << iter << " node=" << node.idx
                  << " op=" << node.op << " out=" << out.str() << "\n";
      }
      if (out.isValid()) {
        values_[node.idx] = out;
      }
    }
    if (exec_.shouldStop()) {
      break;
    }
  }
  return exec_.compare();
}

} // namespace ArtifactSim
