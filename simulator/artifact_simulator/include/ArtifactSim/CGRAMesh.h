#pragma once

#include "ArtifactSim/CGRAArch.h"
#include "ArtifactSim/PE.h"

#include <cstdlib>
#include <vector>

namespace ArtifactSim {

// The physical CGRA: a 2-D grid of ArchPEs wired with N/S/E/W neighbor connections.
// This class is responsible for inter-ArchPE connectivity.
// Data propagation between ArchPEs is modeled via the N/S/E/W topology:
//   NORTH_O(x,y) → SOUTH_I(x,y-1)   [y decreases going north]
//   SOUTH_O(x,y) → NORTH_I(x,y+1)   [y increases going south]
//   EAST_O(x,y)  → WEST_I(x+1,y)
//   WEST_O(x,y)  → EAST_I(x-1,y)
class CGRAMesh {
public:
    explicit CGRAMesh(const CGRAArch &arch);

    int xdim() const { return xdim_; }
    int ydim() const { return ydim_; }

    const ArchPE &pe(int x, int y) const;
    ArchPE &pe(int x, int y);

    // Returns true if a value produced at (x0,y0) can reach (x1,y1) within
    // `cycles` clock cycles, given that up to `maxHopsPerCycle` N/S/E/W hops
    // can be chained in a single cycle (HyCUBE default = 4).
    // Constraint: ceil(manhattan_distance / maxHopsPerCycle) <= cycles.
    // Same-PE access (TREG) requires 0 cycles.
    bool reachable(int x0, int y0, int x1, int y1, int cycles,
                   int maxHopsPerCycle = 4) const {
        int dist = std::abs(x1 - x0) + std::abs(y1 - y0);
        if (dist == 0) return cycles >= 0;
        int cyclesNeeded = (dist + maxHopsPerCycle - 1) / maxHopsPerCycle;
        return cycles >= cyclesNeeded;
    }

private:
    int xdim_, ydim_;
    std::vector<std::vector<ArchPE>> grid_;  // grid_[y][x]
};

} // namespace ArtifactSim
