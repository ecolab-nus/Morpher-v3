#include "ArtifactSim/ArtifactSimulator.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace ArtifactSim {

Simulator::Simulator(DFGGraph graph, Mapping mapping, MemAlloc alloc, TraceImage trace, SimOptions options)
    : SimulatorBase(std::move(graph), std::move(mapping), std::move(alloc),
                    std::move(trace), options) {
  for (const auto &entry : mapping_.placements()) {
    const Placement &p = entry.second;
    if (p.x < 0 || p.x >= options.xdim || p.y < 0 || p.y >= options.ydim) {
      throw std::runtime_error("placement outside mesh for node " + std::to_string(p.node));
    }
  }
}

SimResult Simulator::run() {
  std::vector<Placement> ordered;
  for (const auto &entry : graph_.nodes()) {
    ordered.push_back(Placement{entry.first, 0, 0, entry.second.asap});
  }
  std::sort(ordered.begin(), ordered.end(), [](const Placement &a, const Placement &b) {
    if (a.latency != b.latency) return a.latency < b.latency;
    return a.node < b.node;
  });

  for (int iter = 0; iter < options_.maxIterations; ++iter) {
    previousValues_ = values_;
    values_.clear();
    for (const auto &placement : ordered) {
      if (graph_.nodes().find(placement.node) == graph_.nodes().end()) {
        continue;
      }
      const DFGNode &node = graph_.node(placement.node);
      Value out = executeNode(node);
      if (std::getenv("ARTIFACT_SIM_DEBUG")) {
        std::cerr << "iter=" << iter << " node=" << node.idx
                  << " op=" << node.op << " out=" << out.str() << "\n";
      }
      if (out.isValid()) {
        values_[node.idx] = out;
      }
    }
    if (shouldStop()) {
      break;
    }
  }
  return compare();
}

} // namespace ArtifactSim
