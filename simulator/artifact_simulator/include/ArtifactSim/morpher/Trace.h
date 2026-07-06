#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ArtifactSim/morpher/Memory.h"

namespace ArtifactSim {

struct TraceImage {
  MemoryImage initial;
  std::map<uint32_t, uint8_t> expected;
  std::set<uint32_t> loopStoreExpectedAddresses;
  std::vector<uint32_t> interestedAddresses;
  std::map<int, std::vector<uint32_t>> externalAddresses;
  std::map<int, std::vector<uint32_t>> externalAccessSizes;
  std::map<int, std::vector<uint32_t>> externalLoadValues;
  std::map<int, std::vector<uint32_t>> externalStoreValues;
};

TraceImage parseTrace(const std::string &path, const MemAlloc &alloc);
void parseAddressTrace(const std::string &path, TraceImage &trace);

} // namespace ArtifactSim
