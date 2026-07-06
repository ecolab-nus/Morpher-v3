#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ArtifactSim {

// A scalar or SIMD-vector value flowing through the fabric / DFG.
// Invalid values model "nothing produced" (predicated-off ops, unrouted ports).
class Value {
public:
  Value();
  explicit Value(uint32_t scalar);
  explicit Value(std::vector<uint32_t> lanes);

  bool isVector() const;
  bool isValid() const;
  size_t lanes() const;
  uint32_t scalar() const;
  const std::vector<uint32_t> &vector() const;

  static Value invalid();
  static Value broadcast(uint32_t scalar, size_t lanes);
  std::string str() const;

private:
  bool valid_;
  std::vector<uint32_t> lanes_;
};

} // namespace ArtifactSim
