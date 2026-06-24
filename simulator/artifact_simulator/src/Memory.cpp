#include "ArtifactSim/Memory.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ArtifactSim {

void MemoryImage::setByte(uint32_t address, uint8_t value) {
  bytes_[address] = value;
}

uint8_t MemoryImage::getByte(uint32_t address) const {
  auto it = bytes_.find(address);
  if (it == bytes_.end()) {
    return 0;
  }
  return it->second;
}

Value MemoryImage::load(uint32_t address, int bytes) const {
  uint32_t result = 0;
  for (int i = 0; i < bytes; ++i) {
    result |= static_cast<uint32_t>(getByte(address + i)) << (8 * i);
  }
  return Value(result);
}

void MemoryImage::store(uint32_t address, int bytes, const Value &value) {
  if (!value.isValid()) {
    return;
  }
  uint32_t scalar = value.scalar();
  for (int i = 0; i < bytes; ++i) {
    setByte(address + i, static_cast<uint8_t>((scalar >> (8 * i)) & 0xff));
  }
}

const std::map<uint32_t, uint8_t> &MemoryImage::bytes() const {
  return bytes_;
}

static std::vector<std::string> splitCsv(const std::string &line) {
  std::vector<std::string> parts;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, ',')) {
    parts.push_back(item);
  }
  return parts;
}

MemAlloc parseMemAlloc(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("could not open mem_alloc file: " + path);
  }
  MemAlloc alloc;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto parts = splitCsv(line);
    if (parts.size() < 2 || parts[0] == "var_name" || parts[0] == "var") {
      continue;
    }
    alloc.base[parts[0]] = static_cast<uint32_t>(std::stoul(parts[1]));
  }
  return alloc;
}

} // namespace ArtifactSim
