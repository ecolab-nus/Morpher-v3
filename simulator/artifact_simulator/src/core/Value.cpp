#include "ArtifactSim/core/Value.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace ArtifactSim {

Value::Value() : valid_(false), lanes_() {}

Value::Value(uint32_t scalar) : valid_(true), lanes_{scalar} {}

Value::Value(std::vector<uint32_t> lanes) : valid_(true), lanes_(std::move(lanes)) {
  if (lanes_.empty()) {
    valid_ = false;
  }
}

bool Value::isVector() const { return valid_ && lanes_.size() > 1; }
bool Value::isValid() const { return valid_; }
size_t Value::lanes() const { return lanes_.size(); }

uint32_t Value::scalar() const {
  if (!valid_ || lanes_.empty()) {
    throw std::runtime_error("invalid scalar value");
  }
  return lanes_[0];
}

const std::vector<uint32_t> &Value::vector() const { return lanes_; }

Value Value::invalid() { return Value(); }

Value Value::broadcast(uint32_t scalar, size_t lanes) {
  return Value(std::vector<uint32_t>(std::max<size_t>(lanes, 1), scalar));
}

std::string Value::str() const {
  if (!valid_) {
    return "<invalid>";
  }
  if (lanes_.size() == 1) {
    return std::to_string(lanes_[0]);
  }
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < lanes_.size(); ++i) {
    if (i) {
      os << ",";
    }
    os << lanes_[i];
  }
  os << "]";
  return os.str();
}

} // namespace ArtifactSim
