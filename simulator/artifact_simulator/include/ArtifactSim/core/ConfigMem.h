#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ArtifactSim {

// The compiled "configuration memory" of the fabric: what every PE does in
// each slot of the II-periodic schedule. This is the boundary between the
// generic simulator core and any mapper-specific frontend: a frontend
// compiles its mapping format into a ConfigMem; the Engine executes it.

// One FU operation issue.
struct FUInstr {
  int node = -1;            // frontend-defined operation id (DFG node)
  std::string op;           // opcode, resolved by the frontend
  int startCycle = 0;       // absolute cycle of first issue (pipeline fill)
  int latency = 1;          // cycles until the result appears on resultPort
  int i1Port = -1, i2Port = -1, pPort = -1; // operand ports (-1 = unused)
  int resultPort = -1;      // -1 for ops with no result (stores)
};

// One mux selection: drive `dst` from `src` this slot. `src` -> `dst` must be
// a fabric edge; the compiler guarantees this (that is where faulty-arch
// legality is enforced).
struct RouteSel {
  int dst = -1;
  int src = -1;
  int node = -1;            // which value is expected to ride this route (for diagnostics)
};

// Configuration of a single PE for a single slot.
struct SlotConfig {
  std::vector<RouteSel> routes;   // mux selections, incl. writes into registers
  std::vector<FUInstr> fu;        // usually 0 or 1 issue per FU per slot
};

struct ConfigMem {
  int II = 1;
  // config[peIndex][slot]
  std::vector<std::vector<SlotConfig>> pe;

  void init(size_t peCount, int ii) {
    II = ii;
    pe.assign(peCount, std::vector<SlotConfig>(static_cast<size_t>(ii)));
  }
};

} // namespace ArtifactSim
