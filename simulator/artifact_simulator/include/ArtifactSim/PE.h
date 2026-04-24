#pragma once

#include "ArtifactSim/CGRAArch.h"

#include <string>

namespace ArtifactSim {

// A single processing element in the arch-aware model.
// Knows its grid coordinates and FU capabilities; has no knowledge of topology.
// Named ArchPE to avoid collision with the legacy PE in Mesh.h.
class ArchPE {
public:
    ArchPE(int x, int y, const PEConfig &cfg) : x_(x), y_(y), cfg_(cfg) {}

    int x() const { return x_; }
    int y() const { return y_; }
    const PEConfig &config() const { return cfg_; }

    bool supportsOp(const std::string &op) const {
        return cfg_.supportedOps.empty() || cfg_.supportedOps.count(op) > 0;
    }

private:
    int x_, y_;
    PEConfig cfg_;
};

} // namespace ArtifactSim
