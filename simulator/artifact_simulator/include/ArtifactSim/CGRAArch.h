#pragma once

#include <set>
#include <string>
#include <vector>

namespace ArtifactSim {

struct PEConfig {
    bool hasMem = false;
    int memLatency = 1;
    int simdWidth = 1;
    bool scalarBroadcast = false;
    std::set<std::string> supportedOps;
};

struct SPMConfig {
    int numBanks = 0;
    int bankSize = 0;
};

struct CGRAArch {
    int xdim = 0;
    int ydim = 0;
    std::vector<std::vector<PEConfig>> pe;  // pe[y][x]
    SPMConfig spm;

    static CGRAArch parse(const std::string &jsonPath);
    const PEConfig &peConfig(int x, int y) const;
};

} // namespace ArtifactSim
