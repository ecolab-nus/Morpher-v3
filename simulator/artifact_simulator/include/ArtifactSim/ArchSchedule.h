#pragma once

#include "ArtifactSim/CGRAMesh.h"
#include "ArtifactSim/DFG.h"
#include "ArtifactSim/Mapping.h"

#include <map>
#include <string>
#include <vector>

namespace ArtifactSim {

struct RoutingError {
    int producerNode, consumerNode;
    int producerX, producerY, producerCycle;
    int consumerX, consumerY, consumerCycle;
    int hopsNeeded, cyclesAvailable;
};

class ArchSchedule {
public:
    int II = 1;
    int maxHopsPerCycle = 4;  // matches mapper --hops default

    // Build a schedule from an existing Mapping and validate routing against
    // the mesh for every DFG edge.  II is extracted from the mapping filename
    // (pattern "_II=<n>_") or falls back to the provided defaultII argument.
    // maxHopsPerCycle corresponds to the mapper's --hops flag (default 4).
    // Throws std::runtime_error listing all routing violations found.
    static ArchSchedule build(const Mapping &mapping,
                              const CGRAMesh &mesh,
                              const DFGGraph &graph,
                              int defaultII = 1,
                              int maxHopsPerCycle = 4);

    // Returns the placement for node_id, or nullptr if not found.
    const Placement *placementOf(int nodeId) const;
    std::vector<Placement> latencyOrder() const;

    // Parse II from a mappingwithlatency filename ("..._II=N_...").
    static int parseIIFromFilename(const std::string &path);

private:
    std::map<int, Placement> placements_;
};

} // namespace ArtifactSim
