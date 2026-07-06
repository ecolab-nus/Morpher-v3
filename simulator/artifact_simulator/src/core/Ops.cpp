#include "ArtifactSim/core/Ops.h"

#include <stdexcept>

namespace ArtifactSim {

std::string baseOpName(const std::string &op) {
  if (op.size() > 1 && op[0] == 'V') {
    return op.substr(1);
  }
  return op;
}

namespace {

size_t resultLaneCount(const Value &a, const Value &b) {
  return std::max<size_t>(a.lanes(), b.lanes());
}

uint32_t laneAt(const Value &value, size_t lane) {
  if (value.lanes() == 1) {
    return value.vector()[0];
  }
  if (lane >= value.lanes()) {
    throw std::runtime_error("vector lane count mismatch");
  }
  return value.vector()[lane];
}

uint32_t evalBinary(uint32_t a, uint32_t b, const std::string &baseOp) {
  if (baseOp == "ADD" || baseOp == "GEPADD") return a + b;
  if (baseOp == "SUB") return a - b;
  if (baseOp == "MUL" || baseOp == "GEPSCALE") return a * b;
  if (baseOp == "DIV") return b == 0 ? 0 : a / b;
  if (baseOp == "LS" || baseOp == "GEPLEFTSHIFT") return a << b;
  if (baseOp == "RS") return a >> b;
  if (baseOp == "ARS") return static_cast<uint32_t>(static_cast<int32_t>(a) >> b);
  if (baseOp == "AND") return a & b;
  if (baseOp == "OR") return a | b;
  if (baseOp == "XOR") return a ^ b;
  if (baseOp == "CMP") return a == b ? 1 : 0;
  if (baseOp == "CLT") return a < b ? 1 : 0;
  if (baseOp == "CGT") return a > b ? 1 : 0;
  if (baseOp == "CMERGE") return a != 0 ? a : b;
  if (baseOp == "SELECT") return a != 0 ? a : b;
  throw std::runtime_error("unknown binary op: " + baseOp);
}

} // namespace

bool isKnownAluOp(const std::string &op) {
  static const char *known[] = {
      "ADD", "SUB", "MUL", "DIV", "LS", "RS", "ARS", "AND", "OR", "XOR",
      "CMP", "CLT", "CGT", "CMERGE", "SELECT", "GEPADD", "GEPSCALE",
      "GEPLEFTSHIFT", "SEXT", "MOVC", "MOV", "MAC", "VRED",
  };
  const std::string base = baseOpName(op);
  for (const char *k : known) {
    if (base == k) return true;
  }
  return false;
}

Value unaryOp(const Value &a, const std::string &op) {
  if (!a.isValid()) {
    return Value::invalid();
  }
  const std::string baseOp = baseOpName(op);
  // Identity ops on the 32-bit integer datapath. SEXT is identity because
  // values are stored as uint32_t and interpreted signed by the consumer.
  if (baseOp == "SEXT" || baseOp == "MOVC" || baseOp == "MOV") {
    return a;
  }
  throw std::runtime_error("unknown unary op: " + op);
}

Value binaryOp(const Value &a, const Value &b, const std::string &op) {
  if (!a.isValid() || !b.isValid()) {
    return Value::invalid();
  }
  const std::string baseOp = baseOpName(op);
  size_t lanes = resultLaneCount(a, b);
  std::vector<uint32_t> out;
  out.reserve(lanes);
  for (size_t i = 0; i < lanes; ++i) {
    out.push_back(evalBinary(laneAt(a, i), laneAt(b, i), baseOp));
  }
  return Value(std::move(out));
}

uint32_t reduceLanes(const std::vector<uint32_t> &lanes, const std::string &op) {
  if (lanes.empty()) {
    return 0;
  }
  uint32_t acc = lanes.front();
  for (size_t i = 1; i < lanes.size(); ++i) {
    const uint32_t value = lanes[i];
    if (op == "ADD") {
      acc += value;
    } else if (op == "AND") {
      acc &= value;
    } else if (op == "OR") {
      acc |= value;
    } else if (op == "XOR") {
      acc ^= value;
    } else {
      throw std::runtime_error("unsupported VRED reduction op: " + op);
    }
  }
  return acc;
}

} // namespace ArtifactSim
