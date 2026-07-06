#include "ArtifactSim/core/Engine.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <queue>
#include <stdexcept>

namespace ArtifactSim {

namespace {

// Per-slot combinational network: every destination port has at most one
// driver (a fixed wire for single-fan-in ports, or this slot's mux select).
// Register writes latch at the end of the cycle and are kept separate.
struct SlotNetwork {
  std::vector<RouteSel> ordered;   // comb copies in dependency order
  std::vector<RouteSel> regWrites; // latched after propagation
};

SlotNetwork buildSlotNetwork(const Fabric &fabric, const ConfigMem &config,
                             int slot) {
  std::map<int, RouteSel> driverOf; // dst -> route

  auto addDriver = [&](const RouteSel &route, const char *what) {
    if (!fabric.hasEdge(route.src, route.dst)) {
      throw std::runtime_error(
          std::string("route uses a connection absent from the fabric (") +
          what + "): " + fabric.portName[route.src] + " -> " +
          fabric.portName[route.dst] + " slot " + std::to_string(slot));
    }
    auto inserted = driverOf.emplace(route.dst, route);
    if (!inserted.second) {
      throw std::runtime_error("two drivers for port " +
                               fabric.portName[route.dst] + " in slot " +
                               std::to_string(slot));
    }
  };

  // Fixed wires: non-register ports with exactly one fabric driver forward
  // unconditionally (inter-PE wires, single-source internal hops).
  for (size_t dst = 0; dst < fabric.portCount(); ++dst) {
    if (fabric.portIsReg[dst]) {
      continue;
    }
    const std::set<int> &sources = fabric.fanIn(static_cast<int>(dst));
    if (sources.size() == 1) {
      addDriver(RouteSel{static_cast<int>(dst), *sources.begin(), -1}, "wire");
    }
  }

  SlotNetwork net;
  for (const auto &peSlots : config.pe) {
    const SlotConfig &sc = peSlots[static_cast<size_t>(slot)];
    for (const RouteSel &route : sc.routes) {
      if (fabric.portIsReg[route.dst]) {
        if (!fabric.hasEdge(route.src, route.dst)) {
          throw std::runtime_error(
              "register write uses a connection absent from the fabric: " +
              fabric.portName[route.src] + " -> " + fabric.portName[route.dst]);
        }
        net.regWrites.push_back(route);
      } else {
        addDriver(route, "mux select");
      }
    }
  }

  // Order the copies so every source is computed before its destination.
  // Sources that are registers or FU result ports are roots (their value is
  // present before propagation starts). Each dst has exactly one driver, so
  // this is a forest; a cycle means a combinational loop in the config.
  std::map<int, std::vector<int>> children; // src -> dsts driven from it
  std::map<int, int> indegree;              // dst -> 1 if its src is itself driven
  for (const auto &[dst, route] : driverOf) {
    children[route.src].push_back(dst);
    indegree[dst] = driverOf.count(route.src) ? 1 : 0;
  }
  std::queue<int> ready;
  for (const auto &[dst, deg] : indegree) {
    if (deg == 0) {
      ready.push(dst);
    }
  }
  while (!ready.empty()) {
    int dst = ready.front();
    ready.pop();
    net.ordered.push_back(driverOf.at(dst));
    for (int next : children[dst]) {
      if (--indegree[next] == 0) {
        ready.push(next);
      }
    }
  }
  if (net.ordered.size() != driverOf.size()) {
    throw std::runtime_error("combinational loop in slot " +
                             std::to_string(slot));
  }
  return net;
}

} // namespace

Engine::Engine(const Fabric &fabric, const ConfigMem &config, EngineHooks hooks)
    : fabric_(fabric), config_(config), hooks_(std::move(hooks)) {
  slotWireOrder_.reserve(static_cast<size_t>(config_.II));
  slotRegWrites_.reserve(static_cast<size_t>(config_.II));
  for (int slot = 0; slot < config_.II; ++slot) {
    SlotNetwork net = buildSlotNetwork(fabric_, config_, slot);
    slotWireOrder_.push_back(std::move(net.ordered));
    slotRegWrites_.push_back(std::move(net.regWrites));
  }
}

int Engine::run(int maxCycles) {
  const bool debug = std::getenv("ARTIFACT_SIM_DEBUG") != nullptr;
  std::vector<Value> portValue(fabric_.portCount());

  // Iteration cap: when an execution flips the stop condition ("this was the
  // final iteration K"), every instruction is capped at its own iteration K
  // and the pipeline drains exactly those in-flight executions. A blind stop
  // would drop the final iteration's late stores; a blind drain would let
  // unpredicated stores of iteration K+1 overwrite results (pruned DFGs lost
  // the predication real hardware would use).
  int stopIteration = -1;
  int stopAt = -1;
  auto computeStopAt = [&](int K) {
    int last = 0;
    for (const auto &peSlots : config_.pe) {
      for (const SlotConfig &sc : peSlots) {
        for (const FUInstr &instr : sc.fu) {
          last = std::max(last, instr.startCycle + K * config_.II +
                                    instr.latency);
        }
      }
    }
    return last + 1;
  };

  int cycle = 0;
  for (; cycle < maxCycles; ++cycle) {
    const int slot = cycle % config_.II;

    // Reset the network; registers drive their latched state.
    std::fill(portValue.begin(), portValue.end(), Value::invalid());
    for (const auto &[port, value] : regState_) {
      portValue[static_cast<size_t>(port)] = value;
    }

    // Land FU results scheduled for this cycle.
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->cycle == cycle) {
        portValue[static_cast<size_t>(it->port)] = it->value;
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }

    // Combinational propagation.
    for (const RouteSel &route : slotWireOrder_[static_cast<size_t>(slot)]) {
      portValue[static_cast<size_t>(route.dst)] =
          portValue[static_cast<size_t>(route.src)];
    }

    // FU issues.
    for (size_t peIdx = 0; peIdx < config_.pe.size(); ++peIdx) {
      const SlotConfig &sc = config_.pe[peIdx][static_cast<size_t>(slot)];
      for (const FUInstr &instr : sc.fu) {
        if (cycle < instr.startCycle) {
          continue;
        }
        const int iteration = (cycle - instr.startCycle) / config_.II;
        if (stopIteration >= 0 && iteration > stopIteration) {
          continue;
        }
        auto operand = [&](int port) {
          return port < 0 ? Value::invalid()
                          : portValue[static_cast<size_t>(port)];
        };
        Value out = hooks_.execute(instr, operand(instr.i1Port),
                                   operand(instr.i2Port), operand(instr.pPort),
                                   cycle);
        if (stopIteration < 0 && hooks_.shouldStop && hooks_.shouldStop()) {
          stopIteration = iteration;
          stopAt = computeStopAt(iteration);
        }
        if (debug) {
          std::cerr << "cycle=" << cycle << " pe=" << peIdx
                    << " node=" << instr.node << " op=" << instr.op
                    << " i1=" << operand(instr.i1Port).str()
                    << " i2=" << operand(instr.i2Port).str()
                    << " p=" << operand(instr.pPort).str()
                    << " out=" << out.str() << "\n";
        }
        if (instr.resultPort >= 0) {
          if (instr.latency < 1) {
            throw std::runtime_error("FU latency must be >= 1 for node " +
                                     std::to_string(instr.node));
          }
          pending_.push_back(
              PendingResult{cycle + instr.latency, instr.resultPort,
                            instr.node, out});
        }
      }
    }

    // Latch register writes.
    for (const RouteSel &route : slotRegWrites_[static_cast<size_t>(slot)]) {
      regState_[route.dst] = portValue[static_cast<size_t>(route.src)];
    }

    if (stopAt >= 0 && cycle >= stopAt) {
      ++cycle;
      break;
    }
  }
  return cycle;
}

} // namespace ArtifactSim
