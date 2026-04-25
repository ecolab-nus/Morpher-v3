#include "ArtifactSim/ArchSimulator.h"

#include <algorithm>
#include <iostream>

namespace ArtifactSim {

ArchSimulator::ArchSimulator(CGRAArch arch, ArchSchedule schedule,
                             DFGGraph graph, Mapping mapping,
                             MemAlloc alloc, TraceImage trace, SimOptions options)
    : SimulatorBase(std::move(graph), std::move(mapping), std::move(alloc),
                    std::move(trace), options),
      mesh_(arch),
      schedule_(std::move(schedule)) {
    for (const auto &[nodeId, node] : graph_.nodes()) {
        const Placement *pl = schedule_.placementOf(nodeId);
        if (!pl) continue;
        const ArchPE &pe = mesh_.pe(pl->x, pl->y);
        if (!pe.supportsOp(node.op)) {
            std::cerr << "Warning: node " << nodeId << " op=" << node.op
                      << " is not listed as supported by PE("
                      << pl->x << "," << pl->y << ")\n";
        }
    }
}

SimResult ArchSimulator::run() {
    std::vector<Placement> ordered;
    for (const auto &entry : graph_.nodes()) {
        ordered.push_back(Placement{entry.first, 0, 0, entry.second.asap});
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const Placement &a, const Placement &b) {
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
            if (out.isValid()) {
                values_[node.idx] = out;
            }
        }

        if (shouldStop()) break;
    }

    return compare();
}

} // namespace ArtifactSim
