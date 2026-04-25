#pragma once

#include <map>
#include <string>
#include <vector>

namespace ArtifactSim {

struct Placement {
  int node = -1;
  int x = 0;
  int y = 0;
  int latency = 0;
};

class Mapping {
public:
  static Mapping parse(const std::string &path);

  const std::map<int, Placement> &placements() const;
  std::vector<Placement> latencyOrder() const;
  bool hasNode(int node) const;
  const Placement &placement(int node) const;

private:
  static Mapping parseCsv(std::istream &in);
  static Mapping parseJsonFallback(const std::string &path);
  std::map<int, Placement> placements_;
};

} // namespace ArtifactSim
