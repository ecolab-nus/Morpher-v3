#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ArtifactSim {

struct EdgeRef {
  int idx = -1;
  std::string type;
  bool nextIter = false;
  bool npb = false;
};

struct MemoryAccess {
  std::string base;
  int sizeBytes = 4;
  int gepOffset = 0;
  bool hasGepOffset = false;
};

struct DFGNode {
  int idx = -1;
  int asap = 0;
  int alap = 0;
  std::string op;
  bool hasConst = false;
  uint32_t constValue = 0;
  bool simdPacked = false;
  int simdLanes = 1;
  MemoryAccess mem;
  std::vector<EdgeRef> inputs;
  std::vector<EdgeRef> outputs;
  std::vector<EdgeRef> recParents;
};

class DFGGraph {
public:
  static DFGGraph parse(const std::string &path);

  const std::map<int, DFGNode> &nodes() const;
  const DFGNode &node(int idx) const;

private:
  std::map<int, DFGNode> nodes_;
};

int opAccessSize(const std::string &op);
bool isLoadOp(const std::string &op);
bool isStoreOp(const std::string &op);

} // namespace ArtifactSim
