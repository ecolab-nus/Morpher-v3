#include "ArtifactSim/ArtifactSimulator.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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
  // Schedule nodes by ASAP, but within an ASAP bucket use Kahn's algorithm so
  // a producer (e.g. a synthetic VRED) always precedes its same-ASAP consumer
  // (e.g. the rewritten OSTORE).  A non-transitive comparator would otherwise
  // leave ordering between producer/consumer pairs unspecified.
  std::map<int, std::vector<int>> bucketsByAsap;
  for (const auto &entry : graph_.nodes()) {
    bucketsByAsap[entry.second.asap].push_back(entry.first);
  }

  std::vector<Placement> ordered;
  ordered.reserve(graph_.nodes().size());
  for (auto &kv : bucketsByAsap) {
    const int latency = kv.first;
    std::vector<int> &bucket = kv.second;
    std::unordered_set<int> bucketSet(bucket.begin(), bucket.end());

    std::unordered_map<int, int> indegree;
    indegree.reserve(bucket.size());
    for (int idx : bucket) {
      const DFGNode &node = graph_.node(idx);
      int count = 0;
      for (const auto &in : node.inputs) {
        if (!in.nextIter && bucketSet.count(in.idx)) {
          ++count;
        }
      }
      indegree[idx] = count;
    }

    std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
    for (int idx : bucket) {
      if (indegree[idx] == 0) ready.push(idx);
    }

    size_t emitted = 0;
    while (!ready.empty()) {
      int idx = ready.top();
      ready.pop();
      ordered.push_back(Placement{idx, 0, 0, latency});
      ++emitted;
      const DFGNode &node = graph_.node(idx);
      for (const auto &out : node.outputs) {
        if (!out.nextIter && bucketSet.count(out.idx)) {
          auto it = indegree.find(out.idx);
          if (it != indegree.end() && --(it->second) == 0) {
            ready.push(out.idx);
          }
        }
      }
    }
    // If a cycle exists inside an ASAP bucket (shouldn't happen for a valid
    // DFG), fall back to idx-order for the remainder so we don't drop nodes.
    if (emitted < bucket.size()) {
      for (int idx : bucket) {
        if (indegree[idx] > 0) {
          ordered.push_back(Placement{idx, 0, 0, latency});
        }
      }
    }
  }

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
