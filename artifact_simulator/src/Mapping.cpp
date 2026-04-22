#include "ArtifactSim/Mapping.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ArtifactSim {

static std::vector<std::string> splitCsv(const std::string &line) {
  std::vector<std::string> parts;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, ',')) {
    parts.push_back(item);
  }
  return parts;
}

Mapping Mapping::parse(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("could not open mapping file: " + path);
  }
  Mapping mapping;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto parts = splitCsv(line);
    if (parts.size() < 4) {
      continue;
    }
    try {
      Placement p;
      p.node = std::stoi(parts[0]);
      p.x = std::stoi(parts[1]);
      p.y = std::stoi(parts[2]);
      p.latency = std::stoi(parts[3]);
      mapping.placements_[p.node] = p;
    } catch (const std::exception &) {
      continue;
    }
  }
  return mapping;
}

const std::map<int, Placement> &Mapping::placements() const {
  return placements_;
}

std::vector<Placement> Mapping::latencyOrder() const {
  std::vector<Placement> ordered;
  for (const auto &entry : placements_) {
    ordered.push_back(entry.second);
  }
  std::sort(ordered.begin(), ordered.end(), [](const Placement &a, const Placement &b) {
    if (a.latency != b.latency) return a.latency < b.latency;
    return a.node < b.node;
  });
  return ordered;
}

bool Mapping::hasNode(int node) const {
  return placements_.find(node) != placements_.end();
}

const Placement &Mapping::placement(int node) const {
  auto it = placements_.find(node);
  if (it == placements_.end()) {
    throw std::runtime_error("mapping missing node: " + std::to_string(node));
  }
  return it->second;
}

} // namespace ArtifactSim
