#include "ArtifactSim/Value.h"

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

static size_t resultLaneCount(const Value &a, const Value &b) {
  return std::max<size_t>(a.lanes(), b.lanes());
}

static uint32_t laneAt(const Value &value, size_t lane) {
  if (value.lanes() == 1) {
    return value.vector()[0];
  }
  if (lane >= value.lanes()) {
    throw std::runtime_error("vector lane count mismatch");
  }
  return value.vector()[lane];
}

static uint32_t evalBinary(uint32_t a, uint32_t b, const std::string &op) {
  if (op == "ADD" || op == "GEPADD") return a + b;
  if (op == "SUB") return a - b;
  if (op == "MUL" || op == "GEPSCALE") return a * b;
  if (op == "DIV") return b == 0 ? 0 : a / b;
  if (op == "LS" || op == "GEPLEFTSHIFT") return a << b;
  if (op == "RS" || op == "ARS") return a >> b;
  if (op == "AND") return a & b;
  if (op == "OR") return a | b;
  if (op == "XOR") return a ^ b;
  if (op == "CMP") return a == b ? 1 : 0;
  if (op == "CLT") return a < b ? 1 : 0;
  if (op == "CGT") return a > b ? 1 : 0;
  if (op == "CMERGE") return a != 0 ? a : b;
  if (op == "SELECT") return a != 0 ? a : b;
  return a;
}

Value unaryOp(const Value &a, const std::string &op) {
  if (!a.isValid()) {
    return Value::invalid();
  }
  if (op == "SEXT") {
    return a;
  }
  return a;
}

Value binaryOp(const Value &a, const Value &b, const std::string &op) {
  if (!a.isValid() || !b.isValid()) {
    return Value::invalid();
  }
  size_t lanes = resultLaneCount(a, b);
  std::vector<uint32_t> out;
  out.reserve(lanes);
  for (size_t i = 0; i < lanes; ++i) {
    out.push_back(evalBinary(laneAt(a, i), laneAt(b, i), op));
  }
  return Value(std::move(out));
}

Value selectOp(const Value &predOrData, const Value &fallback) {
  if (predOrData.isValid()) {
    return predOrData;
  }
  return fallback;
}

} // namespace ArtifactSim
