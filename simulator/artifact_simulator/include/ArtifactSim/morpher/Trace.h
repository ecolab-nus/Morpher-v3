#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ArtifactSim/morpher/Memory.h"

namespace ArtifactSim {

struct TraceImage {
  struct Variable {
    std::map<uint32_t, uint8_t> pre;
    std::map<uint32_t, uint8_t> post;
  };

  MemoryImage initial;
  std::map<uint32_t, uint8_t> expected;
  std::set<uint32_t> loopStoreExpectedAddresses;
  std::vector<uint32_t> interestedAddresses;
  std::map<int, std::vector<uint32_t>> externalAddresses;
  std::map<int, std::vector<uint32_t>> externalAccessSizes;
  std::map<int, std::vector<uint32_t>> externalLoadValues;
  std::map<int, std::vector<uint32_t>> externalStoreValues;
  // Raw named variables are retained even when they have no scratchpad
  // allocation. Boundary contracts use these values for host-executed loop
  // preheaders/exits (for example, a scalar accumulator initializer).
  std::map<std::string, Variable> variables;
};

TraceImage parseTrace(const std::string &path, const MemAlloc &alloc);
void parseAddressTrace(const std::string &path, TraceImage &trace);

} // namespace ArtifactSim
