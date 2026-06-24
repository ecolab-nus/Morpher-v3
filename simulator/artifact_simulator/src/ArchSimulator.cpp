#include "ArtifactSim/ArchSimulator.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

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
    std::map<std::pair<int, int>, Value> tregValues;
    return runWithState(tregValues, true);
}

SimResult ArchSimulator::runWithState(
    std::map<std::pair<int, int>, Value> &tregValues,
    bool compareAfter) {
    const std::vector<Placement> ordered = schedule_.latencyOrder();
    if (ordered.empty()) {
        throw std::runtime_error("mapped schedule contains no placements");
    }

    // MAC state is a physical PE register. Multiple phase-unrolled MAC nodes
    // placed on the same PE must therefore observe and update one accumulator.
    // This is intentionally different from the arch-unaware DFG simulator,
    // which models each MAC node's recurrence independently.
    for (int iter = 0; iter < options_.maxIterations; ++iter) {
        previousValues_ = values_;
        values_.clear();

        for (const auto &placement : ordered) {
            if (graph_.nodes().find(placement.node) == graph_.nodes().end()) {
                continue;
            }
            const DFGNode &node = graph_.node(placement.node);
            const bool isMac = node.op == "MAC" || node.op == "VMAC";
            const std::pair<int, int> pe{placement.x, placement.y};
            Value *accumulator = nullptr;
            if (isMac) {
                accumulator = &tregValues[pe];
            }
            Value out = executeNode(node, accumulator);
            if (std::getenv("ARTIFACT_SIM_DEBUG")) {
                std::cerr << "iter=" << iter << " cycle=" << placement.latency
                          << " pe=(" << placement.x << "," << placement.y << ")"
                          << " node=" << node.idx << " op=" << node.op
                          << " out=" << out.str() << "\n";
            }
            if (out.isValid()) {
                values_[node.idx] = out;
                if (isMac) {
                    tregValues[pe] = out;
                }
            }
        }

        if (shouldStop()) break;
    }

    return compareAfter ? compare() : SimResult{};
}

} // namespace ArtifactSim
