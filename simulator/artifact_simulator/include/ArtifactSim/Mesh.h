#pragma once

#include <vector>

#include "ArtifactSim/Mapping.h"

namespace ArtifactSim {

class PE {
public:
  PE(int x, int y);
  int x() const;
  int y() const;
  void add(const Placement &placement);
  const std::vector<Placement> &scheduled() const;

private:
  int x_;
  int y_;
  std::vector<Placement> scheduled_;
};

class Mesh {
public:
  Mesh(int xdim, int ydim);
  void place(const Placement &placement);
  const PE &pe(int x, int y) const;

private:
  int xdim_;
  int ydim_;
  std::vector<PE> pes_;
};

} // namespace ArtifactSim
