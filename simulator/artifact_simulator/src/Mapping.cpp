#include "ArtifactSim/Mapping.h"

#include "morpher/arch/json.hpp"

#include <algorithm>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

namespace ArtifactSim {
using nlohmann::json;

static std::vector<std::string> splitCsv(const std::string &line) {
  std::vector<std::string> parts;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, ',')) {
    parts.push_back(item);
  }
  return parts;
}

Mapping Mapping::parseCsv(std::istream &in) {
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

static bool parsePeCoords(const std::string &key, int &x, int &y) {
  std::smatch m;
  static const std::regex memRe(R"(PE_MEM_X(\d+)_Y(\d+))");
  static const std::regex peRe(R"(PE_X(\d+)\|_Y(\d+)\|)");
  static const std::regex typedPeRe(R"(PE_[A-Z0-9]+_X(\d+)\|_Y(\d+)\|)");
  if (std::regex_search(key, m, memRe)
      || std::regex_search(key, m, peRe)
      || std::regex_search(key, m, typedPeRe)) {
    x = std::stoi(m[1].str());
    y = std::stoi(m[2].str());
    return true;
  }
  static const std::regex fallbackRe(R"(PE_X(\d+)_Y(\d+))");
  static const std::regex typedFallbackRe(R"(PE_[A-Z0-9]+_X(\d+)_Y(\d+))");
  if (!std::regex_search(key, m, fallbackRe)
      && !std::regex_search(key, m, typedFallbackRe)) {
    return false;
  }
  x = std::stoi(m[1].str());
  y = std::stoi(m[2].str());
  return true;
}

Mapping Mapping::parseJsonFallback(const std::string &path) {
  const std::size_t slash = path.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  const std::string mappingName = (slash == std::string::npos) ? path : path.substr(slash + 1);
  DIR *dp = opendir(dir.c_str());
  if (!dp) {
    return Mapping{};
  }
  closedir(dp);

  std::string jsonPath;
  std::smatch m;
  static const std::regex prefixRe(R"((.+?)_MTP=.*\.mappingwithlatency\.txt$)");
  if (std::regex_match(mappingName, m, prefixRe)) {
    const std::string prefix = m[1].str();
    const std::string candidate = dir + "/" + prefix + "mapping_i.txtmapping.json";
    std::ifstream test(candidate);
    if (test.good()) {
      jsonPath = candidate;
    }
  }
  if (jsonPath.empty()) {
    DIR *scan = opendir(dir.c_str());
    if (!scan) {
      return Mapping{};
    }
    std::time_t bestMtime = 0;
    struct dirent *ent = nullptr;
    while ((ent = readdir(scan)) != nullptr) {
      const std::string name(ent->d_name);
      if (name.size() >= 24
          && name.rfind("mapping_i.txtmapping.json") == name.size() - 24) {
        const std::string candidate = dir + "/" + name;
        struct stat st;
        if (stat(candidate.c_str(), &st) != 0) {
          continue;
        }
        if (jsonPath.empty() || st.st_mtime > bestMtime) {
          jsonPath = candidate;
          bestMtime = st.st_mtime;
        }
      }
    }
    closedir(scan);
  }
  if (jsonPath.empty()) {
    return Mapping{};
  }

  std::ifstream jf(jsonPath);
  if (!jf) {
    return Mapping{};
  }

  json doc;
  jf >> doc;

  Mapping mapping;
  std::set<int> opInfoNodes;
  if (!doc.contains("CGRA_INS") || !doc["CGRA_INS"].contains("SUBMODS")) {
    return mapping;
  }

  std::function<void(const json&, int, int)> collectMappedNodesRecursive =
      [&](const json &obj, int x, int y) {
        if (!obj.is_object()) {
          return;
        }
        auto mappedIt = obj.find("MAPPED_NODES");
        if (mappedIt != obj.end() && mappedIt->is_object()) {
          const json &mappedNodes = *mappedIt;
          for (auto cycIt = mappedNodes.begin(); cycIt != mappedNodes.end(); ++cycIt) {
            int latency = 0;
            try {
              latency = std::stoi(cycIt.key());
            } catch (const std::exception &) {
              continue;
            }
            const json &ports = cycIt.value();
            if (!ports.is_object()) {
              continue;
            }
            for (auto portIt = ports.begin(); portIt != ports.end(); ++portIt) {
              if (!portIt.value().is_number_integer()) {
                continue;
              }
              const int node = portIt.value().get<int>();
              if (opInfoNodes.find(node) != opInfoNodes.end()) {
                continue;
              }
              auto existing = mapping.placements_.find(node);
              if (existing == mapping.placements_.end() || latency < existing->second.latency) {
                mapping.placements_[node] = Placement{node, x, y, latency};
              }
            }
          }
        }
        auto opInfoIt = obj.find("OP_INFO");
        if (opInfoIt != obj.end() && opInfoIt->is_object()) {
          const json &opInfo = *opInfoIt;
          for (auto slotIt = opInfo.begin(); slotIt != opInfo.end(); ++slotIt) {
            int latency = 0;
            try {
              latency = std::stoi(slotIt.key());
            } catch (const std::exception &) {
              continue;
            }
            if (!slotIt.value().is_object()) {
              continue;
            }
            auto idIt = slotIt.value().find("id");
            if (idIt == slotIt.value().end() || !idIt->is_number_integer()) {
              continue;
            }
            const int node = idIt->get<int>();
            opInfoNodes.insert(node);
            mapping.placements_[node] = Placement{node, x, y, latency};
          }
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
          if (it.value().is_object()) {
            collectMappedNodesRecursive(it.value(), x, y);
          }
        }
      };

  const json &pes = doc["CGRA_INS"]["SUBMODS"];
  for (auto it = pes.begin(); it != pes.end(); ++it) {
    const std::string peName = it.key();
    int x = 0;
    int y = 0;
    if (!parsePeCoords(peName, x, y)) {
      continue;
    }
    const json &peObj = it.value();
    collectMappedNodesRecursive(peObj, x, y);
  }
  return mapping;
}

Mapping Mapping::parse(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("could not open mapping file: " + path);
  }
  Mapping mapping = parseCsv(in);
  if (!mapping.placements_.empty()) {
    return mapping;
  }
  Mapping fallback = parseJsonFallback(path);
  if (!fallback.placements_.empty()) {
    return fallback;
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
