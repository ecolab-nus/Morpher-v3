#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ArtifactSim {

// The elaborated hardware: a flat directed graph over ports.
//
// A port is any named value-carrying point: PE inputs/outputs (NORTH_I,
// EAST_O), crossbar inputs (NORTH_XBARI), FU operand/result ports
// (FU0.DP0_I1, FU0.DP0_T) and architectural registers (NR, TREG).
//
// Edges come in two kinds:
//   - combinational: value flows within the same cycle (crossbar paths,
//     inter-PE wires -- HyCUBE chains multiple PE hops per cycle),
//   - registered: the destination port is a register; the value written this
//     cycle becomes readable (via the register's outgoing combinational
//     edges) from the next cycle on.
//
// The fabric holds no Morpher-specific knowledge. Faulty architectures are
// simply fabrics with fewer edges: routing legality is a graph property.
struct FabricFU {
  int pe = -1;                 // owning PE index
  std::string name;            // e.g. "FU0"
  int i1 = -1, i2 = -1, p = -1;// operand port ids
  int t = -1;                  // result port id (a persistent register:
                               // holds its value until the next result lands)
  // op -> latency in cycles (issue to result on `t`). Doubles as the
  // supported-op set: an op absent here cannot execute on this FU.
  std::map<std::string, int> opLatency;
};

struct FabricPE {
  int x = 0, y = 0;
  std::string moduleType;      // "PE", "PE_MEM", "PE_Fault_20", ...
  int simdWidth = 1;
  bool scalarBroadcast = false;
  std::vector<int> fus;        // indices into Fabric::fus
  std::map<std::string, int> ports; // local port name -> global port id
};

class Fabric {
public:
  int xdim = 0, ydim = 0;
  bool tregResetOnLaunch = false;

  std::vector<FabricPE> pes;   // index = y * xdim + x
  std::vector<FabricFU> fus;

  // Port tables, indexed by global port id.
  std::vector<std::string> portName; // fully qualified, "PE_X1_Y2.NORTH_XBARI"
  std::vector<int> portPE;           // owning PE index (-1 for fabric-level)
  std::vector<bool> portIsReg;       // register (state element) or wire node

  int peIndex(int x, int y) const { return y * xdim + x; }
  const FabricPE &pe(int x, int y) const { return pes[peIndex(x, y)]; }

  // Returns -1 when the PE has no port of that name.
  int port(int peIdx, const std::string &localName) const;

  int addPort(int peIdx, const std::string &localName, bool isReg);

  void addEdge(int src, int dst) { edges_[src].insert(dst); rev_[dst].insert(src); }
  bool hasEdge(int src, int dst) const;
  const std::set<int> &fanIn(int dst) const;
  const std::set<int> &fanOut(int src) const;

  size_t portCount() const { return portName.size(); }

private:
  std::map<int, std::set<int>> edges_; // src -> dsts
  std::map<int, std::set<int>> rev_;   // dst -> srcs
  static const std::set<int> empty_;
};

} // namespace ArtifactSim
