#include "ArtifactSim/core/Fabric.h"

#include <stdexcept>

namespace ArtifactSim {

const std::set<int> Fabric::empty_{};

int Fabric::port(int peIdx, const std::string &localName) const {
  if (peIdx < 0 || peIdx >= static_cast<int>(pes.size())) {
    return -1;
  }
  const auto &ports = pes[peIdx].ports;
  auto it = ports.find(localName);
  return it == ports.end() ? -1 : it->second;
}

int Fabric::addPort(int peIdx, const std::string &localName, bool isReg) {
  const int id = static_cast<int>(portName.size());
  std::string qualified = localName;
  if (peIdx >= 0) {
    const FabricPE &owner = pes[peIdx];
    qualified = "PE_X" + std::to_string(owner.x) + "_Y" + std::to_string(owner.y)
                + "." + localName;
    auto inserted = pes[peIdx].ports.emplace(localName, id);
    if (!inserted.second) {
      throw std::runtime_error("duplicate port " + qualified);
    }
  }
  portName.push_back(qualified);
  portPE.push_back(peIdx);
  portIsReg.push_back(isReg);
  return id;
}

bool Fabric::hasEdge(int src, int dst) const {
  auto it = edges_.find(src);
  return it != edges_.end() && it->second.count(dst) > 0;
}

const std::set<int> &Fabric::fanIn(int dst) const {
  auto it = rev_.find(dst);
  return it == rev_.end() ? empty_ : it->second;
}

const std::set<int> &Fabric::fanOut(int src) const {
  auto it = edges_.find(src);
  return it == edges_.end() ? empty_ : it->second;
}

} // namespace ArtifactSim
