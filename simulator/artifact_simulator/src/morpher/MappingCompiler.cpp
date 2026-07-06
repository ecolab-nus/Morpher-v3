#include "ArtifactSim/morpher/MappingCompiler.h"

#include "morpher/arch/json.hpp"

#include <dirent.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>

using json = nlohmann::json;

namespace ArtifactSim {

namespace {

// Extract grid coordinates from a mapping.json PE instance key. Handles all
// naming styles the mapper emits: "PE_X1|_Y1|", "PE_MEM_X0_Y1",
// "PE_DUMMY_X0|_Y5|", "PE_Fault_20_X2_Y0", ...
bool parsePeCoords(const std::string &key, int &x, int &y) {
  static const std::regex coordRe(R"(_X(\d+)\|?_Y(\d+)\|?$)");
  std::smatch m;
  if (!std::regex_search(key, m, coordRe)) {
    return false;
  }
  x = std::stoi(m[1].str());
  y = std::stoi(m[2].str());
  return true;
}

struct PEProgram {
  // routes[slot] = raw (src, dst) name pairs from the mapping's CONNECTIONS
  std::map<int, std::vector<std::pair<std::string, std::string>>> routes;
  // occ[slot][rawPortName] = node id (diagnostics)
  std::map<int, std::map<std::string, int>> occ;
  // issue[slot] = node id
  std::map<int, int> issues;
};

int parseSlotKey(const std::string &key, int II, const char *what) {
  int slot = -1;
  try {
    slot = std::stoi(key);
  } catch (const std::exception &) {
    throw std::runtime_error(std::string("non-numeric ") + what +
                             " slot key: " + key);
  }
  if (slot < 0 || slot >= II) {
    throw std::runtime_error(std::string(what) + " slot " + key +
                             " outside II=" + std::to_string(II));
  }
  return slot;
}

// Appends {slot: {src: dst-or-[dsts]}} route records. `rename` optionally
// rewrites port names into the enclosing scope (DP-local "T" -> "DP0_T").
void collectRoutes(const json &connections, int II, PEProgram &out,
                   const std::function<std::string(const std::string &)> &rename) {
  if (!connections.is_object()) {
    return;
  }
  for (auto slotIt = connections.begin(); slotIt != connections.end();
       ++slotIt) {
    const int slot = parseSlotKey(slotIt.key(), II, "CONNECTIONS");
    if (!slotIt.value().is_object()) {
      continue;
    }
    for (auto srcIt = slotIt.value().begin(); srcIt != slotIt.value().end();
         ++srcIt) {
      // Constants are modeled through the DFG node's const operand, not as
      // fabric routes.
      if (srcIt.key().rfind("CONST.", 0) == 0) {
        continue;
      }
      auto add = [&](const std::string &dst) {
        out.routes[slot].emplace_back(rename(srcIt.key()), rename(dst));
      };
      if (srcIt.value().is_string()) {
        add(srcIt.value().get<std::string>());
      } else if (srcIt.value().is_array()) {
        for (const auto &dst : srcIt.value()) {
          add(dst.get<std::string>());
        }
      }
    }
  }
}

PEProgram collectProgram(const json &peObj, const std::string &fuName, int II) {
  PEProgram out;
  auto identity = [](const std::string &name) { return name; };
  if (peObj.contains("CONNECTIONS")) {
    collectRoutes(peObj["CONNECTIONS"], II, out, identity);
  }
  if (peObj.contains("MAPPED_NODES") && peObj["MAPPED_NODES"].is_object()) {
    for (auto slotIt = peObj["MAPPED_NODES"].begin();
         slotIt != peObj["MAPPED_NODES"].end(); ++slotIt) {
      const int slot = parseSlotKey(slotIt.key(), II, "MAPPED_NODES");
      if (!slotIt.value().is_object()) {
        continue;
      }
      for (auto portIt = slotIt.value().begin(); portIt != slotIt.value().end();
           ++portIt) {
        if (portIt.value().is_number_integer()) {
          out.occ[slot][portIt.key()] = portIt.value().get<int>();
        }
      }
    }
  }
  if (peObj.contains("SUBMODS") && peObj["SUBMODS"].contains(fuName)) {
    const json &fuObj = peObj["SUBMODS"][fuName];
    if (fuObj.contains("OP_INFO") && fuObj["OP_INFO"].is_object()) {
      for (auto it = fuObj["OP_INFO"].begin(); it != fuObj["OP_INFO"].end();
           ++it) {
        const int slot = parseSlotKey(it.key(), II, "OP_INFO");
        if (it.value().is_object() && it.value().contains("id")) {
          out.issues[slot] = it.value()["id"].get<int>();
        }
      }
    }
    // FU-level CONNECTIONS only carry FU<->DP plumbing (collapsed in the
    // fabric) and CONST operand feeds (covered by the DFG const fallback).
    // DP-level CONNECTIONS carry one semantic route: the result register's
    // self-forward ("T" -> "I1"), rewritten here into FU-port names.
    if (fuObj.contains("SUBMODS") && fuObj["SUBMODS"].is_object()) {
      for (auto dpIt = fuObj["SUBMODS"].begin(); dpIt != fuObj["SUBMODS"].end();
           ++dpIt) {
        const std::string dpName = dpIt.key();
        if (dpIt.value().contains("CONNECTIONS")) {
          collectRoutes(dpIt.value()["CONNECTIONS"], II, out,
                        [&dpName](const std::string &name) {
                          return dpName + "_" + name;
                        });
        }
      }
    }
  }
  return out;
}

// Resolve a mapping CONNECTIONS port reference to a fabric port.
// "<REG>_RO" (write side) and "<REG>_RI" (read side) both resolve to the
// register port; the caller distinguishes write vs read by position.
// Returns -1 for references the fabric does not model.
int resolveConnRef(const Fabric &fabric, int peIdx, const std::string &raw,
                   const std::string &fuName) {
  for (const char *suffix : {"_RO", "_RI"}) {
    const size_t len = 3;
    if (raw.size() > len && raw.compare(raw.size() - len, len, suffix) == 0) {
      const int port = fabric.port(peIdx, raw.substr(0, raw.size() - len));
      if (port >= 0 && fabric.portIsReg[static_cast<size_t>(port)]) {
        return port;
      }
    }
  }
  int port = fabric.port(peIdx, raw);
  if (port >= 0) {
    return port;
  }
  return fabric.port(peIdx, fuName + "." + raw);
}

} // namespace

std::string findMappingJson(const std::string &mappingPath) {
  const std::size_t slash = mappingPath.find_last_of('/');
  const std::string dir =
      (slash == std::string::npos) ? "." : mappingPath.substr(0, slash);
  const std::string name =
      (slash == std::string::npos) ? mappingPath : mappingPath.substr(slash + 1);

  std::smatch m;
  static const std::regex prefixRe(R"((.+?)_MTP=.*\.mappingwithlatency\.txt$)");
  if (std::regex_match(name, m, prefixRe)) {
    const std::string candidate =
        dir + "/" + m[1].str() + "mapping_i.txtmapping.json";
    std::ifstream test(candidate);
    if (test.good()) {
      return candidate;
    }
  }
  // Fall back to the newest *mapping_i.txtmapping.json in the directory.
  DIR *scan = opendir(dir.c_str());
  if (!scan) {
    return "";
  }
  std::string best;
  std::time_t bestMtime = 0;
  struct dirent *ent = nullptr;
  static const std::string tail = "mapping_i.txtmapping.json";
  while ((ent = readdir(scan)) != nullptr) {
    const std::string entry(ent->d_name);
    if (entry.size() >= tail.size() &&
        entry.rfind(tail) == entry.size() - tail.size()) {
      const std::string candidate = dir + "/" + entry;
      struct stat st;
      if (stat(candidate.c_str(), &st) == 0 &&
          (best.empty() || st.st_mtime > bestMtime)) {
        best = candidate;
        bestMtime = st.st_mtime;
      }
    }
  }
  closedir(scan);
  return best;
}

ConfigMem compileMapping(const Fabric &fabric, const DFGGraph &graph,
                         const std::string &mappingJsonPath, int II) {
  std::ifstream jf(mappingJsonPath);
  if (!jf) {
    throw std::runtime_error("cannot open mapping json: " + mappingJsonPath);
  }
  json doc;
  jf >> doc;
  if (!doc.contains("CGRA_INS") || !doc["CGRA_INS"].contains("SUBMODS")) {
    throw std::runtime_error("mapping json lacks CGRA_INS.SUBMODS: " +
                             mappingJsonPath);
  }

  // ---- 1. Collect each PE's program (routes, occupancy, issues). ----
  std::map<int, PEProgram> programs; // peIdx -> program
  const json &pes = doc["CGRA_INS"]["SUBMODS"];
  for (auto it = pes.begin(); it != pes.end(); ++it) {
    int x = 0, y = 0;
    if (!parsePeCoords(it.key(), x, y)) {
      continue;
    }
    if (x < 0 || x >= fabric.xdim || y < 0 || y >= fabric.ydim) {
      throw std::runtime_error("mapping PE " + it.key() +
                               " outside the fabric grid");
    }
    const int peIdx = fabric.peIndex(x, y);
    const FabricFU &fu = fabric.fus[static_cast<size_t>(
        fabric.pes[static_cast<size_t>(peIdx)].fus[0])];
    programs[peIdx] = collectProgram(it.value(), fu.name, II);
  }

  ConfigMem config;
  config.init(fabric.pes.size(), II);

  // ---- 2. FU issues. ----
  struct IssueInfo {
    int pe;
    int slot;
    int latency;
  };
  std::map<int, IssueInfo> issueOf; // node -> issue
  for (const auto &[peIdx, program] : programs) {
    const FabricPE &pe = fabric.pes[static_cast<size_t>(peIdx)];
    const FabricFU &fu = fabric.fus[static_cast<size_t>(pe.fus[0])];
    for (const auto &[slot, nodeId] : program.issues) {
      if (graph.nodes().find(nodeId) == graph.nodes().end()) {
        throw std::runtime_error("mapping issues unknown DFG node " +
                                 std::to_string(nodeId));
      }
      const DFGNode &node = graph.node(nodeId);
      auto latIt = fu.opLatency.find(node.op);
      if (latIt == fu.opLatency.end()) {
        throw std::runtime_error(
            "node " + std::to_string(nodeId) + " op=" + node.op +
            " is not executable on " + pe.moduleType + " at (" +
            std::to_string(pe.x) + "," + std::to_string(pe.y) + ")");
      }
      FUInstr instr;
      instr.node = nodeId;
      instr.op = node.op;
      instr.latency = latIt->second;
      instr.i1Port = fu.i1;
      instr.i2Port = fu.i2;
      instr.pPort = fu.p;
      instr.resultPort = node.outputs.empty() ? -1 : fu.t;
      config.pe[static_cast<size_t>(peIdx)][static_cast<size_t>(slot)]
          .fu.push_back(instr);
      if (issueOf.count(nodeId)) {
        throw std::runtime_error("node " + std::to_string(nodeId) +
                                 " issued on two FUs");
      }
      issueOf[nodeId] = IssueInfo{peIdx, slot, latIt->second};
    }
  }

  // ---- 3. Start cycles: smallest cycle congruent to the issue slot that
  // satisfies same-iteration data dependencies (pipeline fill order). ----
  std::map<int, int> startCycle;
  for (const auto &[nodeId, info] : issueOf) {
    startCycle[nodeId] = info.slot;
  }
  bool changed = true;
  int rounds = 0;
  while (changed) {
    if (++rounds > static_cast<int>(issueOf.size()) + 8) {
      throw std::runtime_error(
          "start-cycle relaxation did not converge (same-iteration "
          "dependency cycle without nextiter edges?)");
    }
    changed = false;
    for (const auto &[nodeId, info] : issueOf) {
      const DFGNode &node = graph.node(nodeId);
      int ready = 0;
      for (const EdgeRef &in : node.inputs) {
        if (in.nextIter) {
          continue;
        }
        auto prodIt = issueOf.find(in.idx);
        if (prodIt == issueOf.end()) {
          continue;
        }
        ready = std::max(ready,
                         startCycle[in.idx] + prodIt->second.latency);
      }
      int t = startCycle[nodeId];
      while (t < ready) {
        t += II;
      }
      if (t != startCycle[nodeId]) {
        startCycle[nodeId] = t;
        changed = true;
      }
    }
  }
  for (const auto &[nodeId, info] : issueOf) {
    for (auto &instr :
         config.pe[static_cast<size_t>(info.pe)][static_cast<size_t>(info.slot)]
             .fu) {
      if (instr.node == nodeId) {
        instr.startCycle = startCycle[nodeId];
      }
    }
  }

  // ---- 4. Routes: replay the mapper's per-slot CONNECTIONS, validating
  // every hop against the fabric. A hop over a connection the arch JSON does
  // not contain (e.g. faulted out) fails here with full context. ----
  std::set<std::string> unknownRefs;
  int illegal = 0;
  for (const auto &[peIdx, program] : programs) {
    const FabricPE &pe = fabric.pes[static_cast<size_t>(peIdx)];
    const FabricFU &fu = fabric.fus[static_cast<size_t>(pe.fus[0])];
    for (const auto &[slot, hops] : program.routes) {
      for (const auto &[srcRaw, dstRaw] : hops) {
        const int src = resolveConnRef(fabric, peIdx, srcRaw, fu.name);
        const int dst = resolveConnRef(fabric, peIdx, dstRaw, fu.name);
        if (src < 0 || dst < 0) {
          unknownRefs.insert(src < 0 ? srcRaw : dstRaw);
          continue;
        }
        if (dst == fu.t) {
          continue; // FU-internal result path; the engine lands results
        }
        if (!fabric.hasEdge(src, dst)) {
          std::cerr << "Illegal route at PE(" << pe.x << "," << pe.y
                    << ") slot " << slot << ": "
                    << fabric.portName[static_cast<size_t>(src)] << " -> "
                    << fabric.portName[static_cast<size_t>(dst)]
                    << " is not a connection of " << pe.moduleType << "\n";
          ++illegal;
          continue;
        }
        int node = -1;
        auto occSlot = program.occ.find(slot);
        if (occSlot != program.occ.end()) {
          auto occIt = occSlot->second.find(dstRaw);
          if (occIt != occSlot->second.end()) {
            node = occIt->second;
          }
        }
        config.pe[static_cast<size_t>(peIdx)][static_cast<size_t>(slot)]
            .routes.push_back(RouteSel{dst, src, node});
      }
    }
  }
  if (!unknownRefs.empty()) {
    std::cerr << "MappingCompiler: ignoring unmodeled port refs:";
    for (const auto &ref : unknownRefs) {
      std::cerr << " " << ref;
    }
    std::cerr << "\n";
  }
  if (illegal > 0) {
    throw std::runtime_error(
        std::to_string(illegal) +
        " routed connections do not exist in this architecture");
  }

  return config;
}

} // namespace ArtifactSim
