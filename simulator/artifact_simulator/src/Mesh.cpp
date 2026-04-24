#include "ArtifactSim/Mesh.h"

#include <stdexcept>

namespace ArtifactSim {

PE::PE(int x, int y) : x_(x), y_(y), scheduled_() {}
int PE::x() const { return x_; }
int PE::y() const { return y_; }
void PE::add(const Placement &placement) { scheduled_.push_back(placement); }
const std::vector<Placement> &PE::scheduled() const { return scheduled_; }

Mesh::Mesh(int xdim, int ydim) : xdim_(xdim), ydim_(ydim), pes_() {
  for (int y = 0; y < ydim_; ++y) {
    for (int x = 0; x < xdim_; ++x) {
      pes_.emplace_back(x, y);
    }
  }
}

void Mesh::place(const Placement &placement) {
  if (placement.x < 0 || placement.x >= xdim_ || placement.y < 0 || placement.y >= ydim_) {
    throw std::runtime_error("placement outside mesh for node " + std::to_string(placement.node));
  }
  pes_[placement.y * xdim_ + placement.x].add(placement);
}

const PE &Mesh::pe(int x, int y) const {
  if (x < 0 || x >= xdim_ || y < 0 || y >= ydim_) {
    throw std::runtime_error("PE coordinate outside mesh");
  }
  return pes_[y * xdim_ + x];
}

} // namespace ArtifactSim
