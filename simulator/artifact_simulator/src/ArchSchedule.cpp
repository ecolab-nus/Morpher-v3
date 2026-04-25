#include "ArtifactSim/ArchSchedule.h"

#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ArtifactSim {

int ArchSchedule::parseIIFromFilename(const std::string &path) {
    std::regex re("_II=(\\d+)_");
    std::smatch m;
    if (std::regex_search(path, m, re)) {
        return std::stoi(m[1].str());
    }
    return 0;  // not found
}

ArchSchedule ArchSchedule::build(const Mapping &mapping,
                                  const CGRAMesh &mesh,
                                  const DFGGraph &graph,
                                  int defaultII,
                                  int maxHopsPerCycle) {
    ArchSchedule sched;
    sched.placements_ = mapping.placements();
    sched.II = defaultII < 1 ? 1 : defaultII;
    sched.maxHopsPerCycle = maxHopsPerCycle < 1 ? 1 : maxHopsPerCycle;

    // Validate routing for every DFG edge.
    std::vector<RoutingError> errors;
    for (const auto &[nodeId, node] : graph.nodes()) {
        const Placement *prodPl = sched.placementOf(nodeId);
        if (!prodPl) continue;

        for (const auto &out : node.outputs) {
            const Placement *consPl = sched.placementOf(out.idx);
            if (!consPl) continue;

            // Latency values may span multiple II periods (the MRRG can have
            // more than II time steps for multi-stage pipelines).  Reduce both
            // latencies to their position within one II window before computing
            // the hop budget, so the check is against the physical routing
            // registers that must carry the value within a single II.
            int prodCycle = prodPl->latency % sched.II;
            int consCycle = consPl->latency % sched.II;

            int cycleGap;
            if (out.nextIter || consCycle <= prodCycle) {
                // Value crosses an II boundary (either explicitly marked, or
                // consumer slot comes before producer slot within the II).
                cycleGap = sched.II - prodCycle + consCycle;
            } else {
                cycleGap = consCycle - prodCycle;
            }

            if (!mesh.reachable(prodPl->x, prodPl->y,
                                consPl->x, consPl->y, cycleGap,
                                sched.maxHopsPerCycle)) {
                RoutingError err;
                err.producerNode  = nodeId;
                err.consumerNode  = out.idx;
                err.producerX     = prodPl->x;
                err.producerY     = prodPl->y;
                err.producerCycle = prodPl->latency;
                err.consumerX     = consPl->x;
                err.consumerY     = consPl->y;
                err.consumerCycle = consPl->latency;
                err.hopsNeeded    = std::abs(consPl->x - prodPl->x)
                                  + std::abs(consPl->y - prodPl->y);
                err.cyclesAvailable = cycleGap;
                errors.push_back(err);
            }
        }
    }

    if (!errors.empty()) {
        std::ostringstream oss;
        oss << "Routing validation failed for " << errors.size() << " edge(s):\n";
        for (const auto &e : errors) {
            oss << "  node " << e.producerNode
                << " @ PE(" << e.producerX << "," << e.producerY
                << ") cycle=" << e.producerCycle
                << " -> node " << e.consumerNode
                << " @ PE(" << e.consumerX << "," << e.consumerY
                << ") cycle=" << e.consumerCycle
                << " : needs " << e.hopsNeeded
                << " hops but only " << e.cyclesAvailable << " cycle(s) available\n";
        }
        throw std::runtime_error(oss.str());
    }

    // Warn about nodes in the DFG that have no placement.
    for (const auto &[nodeId, node] : graph.nodes()) {
        if (!sched.placementOf(nodeId)) {
            std::cerr << "Warning: DFG node " << nodeId
                      << " (" << node.op << ") has no placement in mapping\n";
        }
    }

    return sched;
}

const Placement *ArchSchedule::placementOf(int nodeId) const {
    auto it = placements_.find(nodeId);
    if (it == placements_.end()) return nullptr;
    return &it->second;
}

std::vector<Placement> ArchSchedule::latencyOrder() const {
    std::vector<Placement> ordered;
    ordered.reserve(placements_.size());
    for (const auto &entry : placements_) {
        ordered.push_back(entry.second);
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const Placement &a, const Placement &b) {
            if (a.latency != b.latency) return a.latency < b.latency;
            return a.node < b.node;
        });
    return ordered;
}

} // namespace ArtifactSim
