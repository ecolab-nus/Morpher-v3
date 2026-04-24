#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ArtifactSim {

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

Value unaryOp(const Value &a, const std::string &op);
Value binaryOp(const Value &a, const Value &b, const std::string &op);

} // namespace ArtifactSim
