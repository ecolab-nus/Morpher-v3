#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include "ArtifactSim/Value.h"

namespace ArtifactSim {

struct MemAlloc {
  std::unordered_map<std::string, uint32_t> base;
};

class MemoryImage {
public:
  void setByte(uint32_t address, uint8_t value);
  uint8_t getByte(uint32_t address) const;

  Value load(uint32_t address, int bytes) const;
  void store(uint32_t address, int bytes, const Value &value);

  const std::map<uint32_t, uint8_t> &bytes() const;

private:
  std::map<uint32_t, uint8_t> bytes_;
};

MemAlloc parseMemAlloc(const std::string &path);

} // namespace ArtifactSim
