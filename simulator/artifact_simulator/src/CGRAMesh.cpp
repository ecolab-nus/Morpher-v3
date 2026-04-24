#include "ArtifactSim/CGRAMesh.h"

#include <stdexcept>

namespace ArtifactSim {

CGRAMesh::CGRAMesh(const CGRAArch &arch) : xdim_(arch.xdim), ydim_(arch.ydim) {
    grid_.reserve(ydim_);
    for (int y = 0; y < ydim_; ++y) {
        grid_.emplace_back();
        grid_.back().reserve(xdim_);
        for (int x = 0; x < xdim_; ++x) {
            grid_.back().emplace_back(x, y, arch.peConfig(x, y));
        }
    }
}

const ArchPE &CGRAMesh::pe(int x, int y) const {
    if (x < 0 || x >= xdim_ || y < 0 || y >= ydim_) {
        throw std::out_of_range("PE coordinates out of range");
    }
    return grid_[y][x];
}

ArchPE &CGRAMesh::pe(int x, int y) {
    if (x < 0 || x >= xdim_ || y < 0 || y >= ydim_) {
        throw std::out_of_range("PE coordinates out of range");
    }
    return grid_[y][x];
}

} // namespace ArtifactSim
