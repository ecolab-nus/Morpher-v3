#pragma once

#include "ArtifactSim/core/ConfigMem.h"
#include "ArtifactSim/core/Fabric.h"
#include "ArtifactSim/core/Value.h"

#include <functional>
#include <map>
#include <vector>

namespace ArtifactSim {

// Callbacks through which the generic engine talks to the frontend.
// The engine knows how values move; the frontend knows what ops mean
// (memory, traces, predication) and when the program is done.
struct EngineHooks {
  // Execute one FU issue. Operand values were sampled from the instr's
  // operand ports this cycle. Return the result (invalid = no result,
  // e.g. predicated off or a store).
  std::function<Value(const FUInstr &, const Value &i1, const Value &i2,
                      const Value &p, int cycle)>
      execute;
  // Polled at every iteration boundary (cycle % II == II-1).
  std::function<bool()> shouldStop;
};

// Cycle-accurate synchronous engine.
//
// Per cycle:
//   1. deliver FU results scheduled to land this cycle onto their result ports,
//   2. propagate combinationally: fixed wires (single-fan-in ports) always
//      forward; mux'd ports follow this slot's RouteSel entries,
//   3. issue FU ops for this slot (operands sampled after propagation),
//   4. latch register writes (visible from the next cycle).
//
// Register state is the only value that survives a cycle boundary; everything
// else is rederived from routes each cycle, so a route through a missing
// (faulty) edge can never be smuggled in by stale state.
class Engine {
public:
  Engine(const Fabric &fabric, const ConfigMem &config, EngineHooks hooks);

  // Runs until shouldStop() or maxCycles. Returns cycles executed.
  int run(int maxCycles);

  // Register state survives across phases (multi-phase kernels).
  std::map<int, Value> &registerState() { return regState_; }

private:
  struct PendingResult {
    int cycle;
    int port;
    int node;
    Value value;
  };

  const Fabric &fabric_;
  const ConfigMem &config_;
  EngineHooks hooks_;
  std::map<int, Value> regState_;          // reg port id -> latched value
  std::vector<PendingResult> pending_;     // in-flight FU results
  // Precomputed per slot: combinational copies in dependency order, and
  // register writes (latched after propagation).
  std::vector<std::vector<RouteSel>> slotWireOrder_;
  std::vector<std::vector<RouteSel>> slotRegWrites_;
};

} // namespace ArtifactSim
