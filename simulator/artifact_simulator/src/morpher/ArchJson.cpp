#include "ArtifactSim/morpher/ArchJson.h"

#include "morpher/arch/json.hpp"

#include <fstream>
#include <regex>
#include <stdexcept>

using json = nlohmann::json;

namespace ArtifactSim {

namespace {

// Resolve a CONNECTIONS reference ("THIS.NORTH_I", "FU0.DP0_T") to the local
// port name used in the fabric ("NORTH_I", "FU0.DP0_T").
std::string localPortName(const std::string &ref) {
  const auto dot = ref.find('.');
  if (dot == std::string::npos) {
    return ref;
  }
  const std::string scope = ref.substr(0, dot);
  const std::string rest = ref.substr(dot + 1);
  return scope == "THIS" ? rest : scope + "." + rest;
}

void addStringList(const json &module, const char *key,
                   std::vector<std::string> &out) {
  if (!module.contains(key)) {
    return;
  }
  for (const auto &entry : module[key]) {
    out.push_back(entry.get<std::string>());
  }
}

bool parseMemoryPeRef(const std::string &ref, int &x, int &y) {
  static const std::regex pattern(
      R"(^PE_MEM_X(-?[0-9]+)_Y(-?[0-9]+)\.MEMPORT$)");
  std::smatch match;
  if (!std::regex_match(ref, match, pattern)) {
    return false;
  }
  x = std::stoi(match[1].str());
  y = std::stoi(match[2].str());
  return true;
}

MemoryEndpoint parseMemoryEndpoint(const std::string &ref,
                                   const std::string &jsonPath) {
  const auto dot = ref.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= ref.size()) {
    throw std::runtime_error("invalid SPM endpoint '" + ref + "' in " +
                             jsonPath);
  }
  return MemoryEndpoint{ref.substr(0, dot), ref.substr(dot + 1)};
}

} // namespace

Fabric elaborateArchJson(const std::string &jsonPath) {
  std::ifstream f(jsonPath);
  if (!f.is_open()) {
    throw std::runtime_error("cannot open arch JSON: " + jsonPath);
  }
  json root;
  f >> root;
  const json &arch = root.at("ARCH");

  // Locate the grid.
  const json *gridEntry = nullptr;
  for (const auto &entry : arch.at("CGRA").at("SUBMODS")) {
    if (entry.value("PATTERN", std::string{}) == "GRID") {
      gridEntry = &entry;
      break;
    }
  }
  if (!gridEntry) {
    throw std::runtime_error("no GRID pattern in CGRA.SUBMODS: " + jsonPath);
  }

  Fabric fabric;
  if (arch.contains("CAPABILITIES")) {
    const json &capabilities = arch.at("CAPABILITIES");
    if (!capabilities.is_object()) {
      throw std::runtime_error("ARCH.CAPABILITIES must be an object: " + jsonPath);
    }
    if (capabilities.contains("TREG_RESET_ON_LAUNCH") &&
        !capabilities.at("TREG_RESET_ON_LAUNCH").is_boolean()) {
      throw std::runtime_error(
          "ARCH.CAPABILITIES.TREG_RESET_ON_LAUNCH must be boolean: " + jsonPath);
    }
    fabric.tregResetOnLaunch =
        capabilities.value("TREG_RESET_ON_LAUNCH", false);
  }
  fabric.xdim = (*gridEntry)["DIMS"]["X"].get<int>();
  fabric.ydim = (*gridEntry)["DIMS"]["Y"].get<int>();

  // Per-tile module type: default PE, overridden by MODS entries.
  std::vector<std::string> tileType(
      static_cast<size_t>(fabric.xdim * fabric.ydim), "PE");
  if (gridEntry->contains("MODS")) {
    for (const auto &mod : (*gridEntry)["MODS"]) {
      const int x = mod["X"].get<int>();
      const int y = mod["Y"].get<int>();
      if (x < 0 || x >= fabric.xdim || y < 0 || y >= fabric.ydim) {
        continue;
      }
      tileType[static_cast<size_t>(y * fabric.xdim + x)] =
          mod.value("MOD", std::string("PE"));
    }
  }

  fabric.pes.resize(static_cast<size_t>(fabric.xdim * fabric.ydim));

  for (int y = 0; y < fabric.ydim; ++y) {
    for (int x = 0; x < fabric.xdim; ++x) {
      const int peIdx = fabric.peIndex(x, y);
      FabricPE &pe = fabric.pes[static_cast<size_t>(peIdx)];
      pe.x = x;
      pe.y = y;
      pe.moduleType = tileType[static_cast<size_t>(peIdx)];
      if (!arch.contains(pe.moduleType)) {
        throw std::runtime_error("arch JSON has no module definition for " +
                                 pe.moduleType);
      }
      const json &peDef = arch[pe.moduleType];
      pe.simdWidth = peDef.value("SIMD_WIDTH", 1);
      pe.scalarBroadcast = peDef.value("SCALAR_BROADCAST", false);

      // PE-local ports.
      std::vector<std::string> wires, regs;
      addStringList(peDef, "INPUTS", wires);
      addStringList(peDef, "OUTPUTS", wires);
      addStringList(peDef, "INTERNALS", wires);
      addStringList(peDef, "REGS", regs);
      for (const auto &name : wires) {
        fabric.addPort(peIdx, name, /*isReg=*/false);
      }
      for (const auto &name : regs) {
        fabric.addPort(peIdx, name, /*isReg=*/true);
      }

      // FU submodules: operand ports are wires, the result port is a
      // persistent register (HyCUBE RES: holds until the next result).
      if (peDef.contains("SUBMODS")) {
        for (auto it = peDef["SUBMODS"].begin(); it != peDef["SUBMODS"].end();
             ++it) {
          const std::string &fuType = it.key();
          const json &fuDef = arch.at(fuType);
          if (!fuDef.contains("OPS")) {
            throw std::runtime_error("submodule type " + fuType +
                                     " has no OPS; not an FU?");
          }
          for (const auto &instance : it.value()) {
            const std::string fuName = instance.at("name").get<std::string>();
            FabricFU fu;
            fu.pe = peIdx;
            fu.name = fuName;
            for (const auto &portEntry : fuDef.at("INPUTS")) {
              const std::string port = portEntry.get<std::string>();
              const int id =
                  fabric.addPort(peIdx, fuName + "." + port, /*isReg=*/false);
              if (port.size() >= 3 && port.rfind("_I1") == port.size() - 3) {
                fu.i1 = id;
              } else if (port.size() >= 3 && port.rfind("_I2") == port.size() - 3) {
                fu.i2 = id;
              } else if (port.size() >= 2 && port.rfind("_P") == port.size() - 2) {
                fu.p = id;
              }
            }
            for (const auto &portEntry : fuDef.at("OUTPUTS")) {
              const std::string port = portEntry.get<std::string>();
              const int id =
                  fabric.addPort(peIdx, fuName + "." + port, /*isReg=*/true);
              if (port.size() >= 2 && port.rfind("_T") == port.size() - 2) {
                fu.t = id;
              }
            }
            if (fu.i1 < 0 || fu.i2 < 0 || fu.p < 0 || fu.t < 0) {
              throw std::runtime_error("FU " + fuName + " of type " + fuType +
                                       " lacks I1/I2/P/T ports");
            }
            // Result-register self-forward (mapper MRRG: DP T -> {P, I1, I2},
            // see Morpher_CGRA_Mapper/src/arch/FU.cpp): chains a same-PE
            // producer's result into the next op's operands.
            fabric.addEdge(fu.t, fu.i1);
            fabric.addEdge(fu.t, fu.i2);
            fabric.addEdge(fu.t, fu.p);
            for (auto opIt = fuDef["OPS"].begin(); opIt != fuDef["OPS"].end();
                 ++opIt) {
              fu.opLatency[opIt.key()] = opIt.value().get<int>();
            }
            pe.fus.push_back(static_cast<int>(fabric.fus.size()));
            fabric.fus.push_back(std::move(fu));
          }
        }
      }
      if (pe.fus.size() != 1) {
        throw std::runtime_error("expected exactly one FU in " +
                                 pe.moduleType + ", found " +
                                 std::to_string(pe.fus.size()));
      }
    }
  }

  // External memory wiring is not part of the value-routing fabric, but it
  // determines which physical SPM bank/port services a memory PE. Preserve
  // every destination: one means rigid wiring, more than one is a crossbar or
  // otherwise ambiguous endpoint that the profiler must not guess between.
  const json &cgra = arch.at("CGRA");
  if (cgra.contains("CONNECTIONS")) {
    for (auto it = cgra.at("CONNECTIONS").begin();
         it != cgra.at("CONNECTIONS").end(); ++it) {
      int x = 0, y = 0;
      if (!parseMemoryPeRef(it.key(), x, y)) {
        continue;
      }
      if (x < 0 || x >= fabric.xdim || y < 0 || y >= fabric.ydim) {
        throw std::runtime_error("memory PE connection outside grid: " +
                                 it.key() + " in " + jsonPath);
      }
      FabricPE &pe = fabric.pes[static_cast<size_t>(fabric.peIndex(x, y))];
      for (const auto &dst : it.value()) {
        pe.memoryEndpoints.push_back(
            parseMemoryEndpoint(dst.get<std::string>(), jsonPath));
      }
    }
  }

  // Intra-PE edges from module CONNECTIONS. This is where faulty archs
  // diverge from healthy ones: a removed connection is simply never added,
  // and any mapping that needs it fails config legalization.
  for (int y = 0; y < fabric.ydim; ++y) {
    for (int x = 0; x < fabric.xdim; ++x) {
      const int peIdx = fabric.peIndex(x, y);
      const json &peDef = arch[fabric.pes[static_cast<size_t>(peIdx)].moduleType];
      if (!peDef.contains("CONNECTIONS")) {
        continue;
      }
      for (auto it = peDef["CONNECTIONS"].begin();
           it != peDef["CONNECTIONS"].end(); ++it) {
        const int src = fabric.port(peIdx, localPortName(it.key()));
        if (src < 0) {
          continue; // e.g. MEMPORT sockets we do not model
        }
        for (const auto &dstRef : it.value()) {
          const int dst =
              fabric.port(peIdx, localPortName(dstRef.get<std::string>()));
          if (dst < 0) {
            continue;
          }
          fabric.addEdge(src, dst);
        }
      }
    }
  }

  // Inter-PE grid wires, replicating the mapper's conventions:
  // bounded grid, NORTH = y-1, links are combinational.
  for (int y = 0; y < fabric.ydim; ++y) {
    for (int x = 0; x < fabric.xdim; ++x) {
      const int peIdx = fabric.peIndex(x, y);
      auto wire = [&](const char *outName, int nx, int ny, const char *inName) {
        if (nx < 0 || nx >= fabric.xdim || ny < 0 || ny >= fabric.ydim) {
          return;
        }
        const int src = fabric.port(peIdx, outName);
        const int dst = fabric.port(fabric.peIndex(nx, ny), inName);
        if (src >= 0 && dst >= 0) {
          fabric.addEdge(src, dst);
        }
      };
      wire("WEST_O", x - 1, y, "EAST_I");
      wire("EAST_O", x + 1, y, "WEST_I");
      wire("NORTH_O", x, y - 1, "SOUTH_I");
      wire("SOUTH_O", x, y + 1, "NORTH_I");
    }
  }

  return fabric;
}

} // namespace ArtifactSim
