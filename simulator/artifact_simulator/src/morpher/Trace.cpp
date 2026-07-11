#include "ArtifactSim/morpher/Trace.h"

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

TraceImage parseTrace(const std::string &path, const MemAlloc &alloc) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("could not open trace file: " + path);
  }

  TraceImage trace;
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto parts = splitCsv(line);
    if (first) {
      first = false;
      if (!parts.empty() && (parts[0] == "addr" || parts[0] == "var_name")) {
        continue;
      }
    }

    uint32_t address = 0;
    uint32_t pre = 0;
    uint32_t post = 0;
    if (parts.size() >= 4) {
      const uint32_t offset = static_cast<uint32_t>(std::stoul(parts[1]));
      pre = static_cast<uint32_t>(std::stoul(parts[2]));
      post = static_cast<uint32_t>(std::stoul(parts[3]));
      trace.variables[parts[0]].pre[offset] = static_cast<uint8_t>(pre & 0xff);
      trace.variables[parts[0]].post[offset] = static_cast<uint8_t>(post & 0xff);
      auto baseIt = alloc.base.find(parts[0]);
      if (baseIt == alloc.base.end()) {
        continue;
      }
      address = baseIt->second + offset;
    } else if (parts.size() >= 3) {
      address = static_cast<uint32_t>(std::stoul(parts[0]));
      pre = static_cast<uint32_t>(std::stoul(parts[1]));
      post = static_cast<uint32_t>(std::stoul(parts[2]));
    } else {
      continue;
    }

    trace.initial.setByte(address, static_cast<uint8_t>(pre & 0xff));
    trace.expected[address] = static_cast<uint8_t>(post & 0xff);
    trace.interestedAddresses.push_back(address);
  }
  return trace;
}

void parseAddressTrace(const std::string &path, TraceImage &trace) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("could not open address trace file: " + path);
  }

  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto parts = splitCsv(line);
    if (first) {
      first = false;
      if (!parts.empty() && parts[0] == "node_idx") {
        continue;
      }
    }
    if (parts.size() < 3) {
      continue;
    }
    int nodeIdx = std::stoi(parts[0]);
    uint32_t address = static_cast<uint32_t>(std::stoul(parts[2]));
    uint32_t accessSize = 0;
    if (parts.size() >= 4) {
      accessSize = static_cast<uint32_t>(std::stoul(parts[3]));
    }
    trace.externalAddresses[nodeIdx].push_back(address);
    trace.externalAccessSizes[nodeIdx].push_back(accessSize);
    if (parts.size() >= 7) {
      bool isStore = false;
      if (parts.size() >= 5) {
        isStore = std::stoul(parts[4]) != 0;
      }
      uint32_t scalar = 0;
      std::vector<uint32_t> lanes;
      if (accessSize > 4 && accessSize % 4 == 0) {
        lanes.resize(accessSize / 4, 0);
      }
      for (size_t i = 6; i < parts.size(); ++i) {
        if (parts[i].empty()) {
          continue;
        }
        uint8_t byte = static_cast<uint8_t>(std::stoul(parts[i]) & 0xff);
        uint32_t byteAddress = address + static_cast<uint32_t>(i - 6);
        if (isStore) {
          trace.expected[byteAddress] = byte;
          trace.loopStoreExpectedAddresses.insert(byteAddress);
        } else {
          trace.expected.erase(byteAddress);
        }
        if (i - 6 < 4) {
          scalar |= static_cast<uint32_t>(byte) << (8 * (i - 6));
        }
        if (!lanes.empty()) {
          size_t lane = (i - 6) / 4;
          size_t laneByte = (i - 6) % 4;
          if (lane < lanes.size()) {
            lanes[lane] |= static_cast<uint32_t>(byte) << (8 * laneByte);
          }
        }
      }
      if (isStore) {
        if (!lanes.empty()) {
          for (uint32_t laneValue : lanes) {
            trace.externalStoreValues[nodeIdx].push_back(laneValue);
          }
        } else if (accessSize <= 4) {
          trace.externalStoreValues[nodeIdx].push_back(scalar);
        }
      } else {
        if (!lanes.empty()) {
          for (uint32_t laneValue : lanes) {
            trace.externalLoadValues[nodeIdx].push_back(laneValue);
          }
        } else if (accessSize <= 4) {
          trace.externalLoadValues[nodeIdx].push_back(scalar);
        }
      }
    }
  }
}

} // namespace ArtifactSim
