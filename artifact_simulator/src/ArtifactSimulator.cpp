#include "ArtifactSim/ArtifactSimulator.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace ArtifactSim {

Simulator::Simulator(DFGGraph graph, Mapping mapping, MemAlloc alloc, TraceImage trace, SimOptions options)
    : graph_(std::move(graph)),
      mapping_(std::move(mapping)),
      alloc_(std::move(alloc)),
      trace_(std::move(trace)),
      options_(options),
      mesh_(options.xdim, options.ydim),
      values_(),
      previousValues_(),
      implicitOffsets_(),
      externalAddressOffsets_() {
  for (const auto &entry : mapping_.placements()) {
    mesh_.place(entry.second);
  }
}

Value Simulator::inputValue(const DFGNode &node, size_t index) const {
  if (index >= node.inputs.size()) {
    if (node.hasConst) {
      return node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
    }
    return Value::invalid();
  }
  int inputIdx = node.inputs[index].idx;
  if (node.inputs[index].nextIter) {
    auto prevIt = previousValues_.find(inputIdx);
    if (prevIt != previousValues_.end()) {
      return prevIt->second;
    }
  } else {
    auto it = values_.find(inputIdx);
    if (it != values_.end()) {
      return it->second;
    }
  }
  return Value::invalid();
}

Value Simulator::inputByType(const DFGNode &node, const std::string &type) const {
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    if (node.inputs[i].type == type) {
      return inputValue(node, i);
    }
  }
  return Value::invalid();
}

Value Simulator::firstDataInput(const DFGNode &node) const {
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    const std::string &type = node.inputs[i].type;
    if (type == "P" || type == "PS") {
      continue;
    }
    Value value = inputValue(node, i);
    if (value.isValid()) {
      return value;
    }
  }
  return Value::invalid();
}

Value Simulator::secondDataInput(const DFGNode &node) const {
  bool seenFirst = false;
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    const std::string &type = node.inputs[i].type;
    if (type == "P" || type == "PS") {
      continue;
    }
    Value value = inputValue(node, i);
    if (!value.isValid()) {
      continue;
    }
    if (!seenFirst) {
      seenFirst = true;
      continue;
    }
    return value;
  }
  return Value::invalid();
}

bool Simulator::predicateAllows(const DFGNode &node) const {
  bool sawPredicate = false;
  bool allowed = false;
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    const EdgeRef &edge = node.inputs[i];
    if (edge.type != "P" && edge.type != "PS") {
      continue;
    }
    sawPredicate = true;
    Value value = inputValue(node, i);
    bool pred = value.isValid() && value.scalar() != 0;
    if (edge.npb) {
      pred = !pred;
    }
    allowed = allowed || pred;
  }
  return !sawPredicate || allowed;
}

std::vector<uint32_t> Simulator::memoryAddresses(const DFGNode &node) {
  auto externalIt = trace_.externalAddresses.find(node.idx);
  if (externalIt != trace_.externalAddresses.end()) {
    size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : 1;
    size_t &cursor = externalAddressOffsets_[node.idx];
    if (cursor + lanes <= externalIt->second.size()) {
      std::vector<uint32_t> addresses;
      addresses.reserve(lanes);
      for (size_t lane = 0; lane < lanes; ++lane) {
        addresses.push_back(externalIt->second[cursor++]);
      }
      return addresses;
    }
  }

  uint32_t base = 0;
  auto baseIt = alloc_.base.find(node.mem.base);
  if (baseIt != alloc_.base.end()) {
    base = baseIt->second;
  }

  uint32_t offset = 0;
  bool explicitOffset = false;
  if (node.mem.hasGepOffset) {
    offset += static_cast<uint32_t>(node.mem.gepOffset);
    explicitOffset = true;
  } else if (node.hasConst && !node.mem.base.empty()) {
    explicitOffset = true;
  } else if (node.hasConst && node.mem.base.empty()) {
    offset += node.constValue;
    explicitOffset = true;
  } else {
    Value dynamicOffset = inputByType(node, "I2");
    if (!dynamicOffset.isValid() && isLoadOp(node.op)) {
      dynamicOffset = firstDataInput(node);
    }
    if (dynamicOffset.isValid()) {
      explicitOffset = true;
      size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : dynamicOffset.lanes();
      std::vector<uint32_t> addresses;
      for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t laneOffset = dynamicOffset.isVector() ? dynamicOffset.vector()[lane] : dynamicOffset.scalar();
        addresses.push_back(laneOffset);
      }
      return addresses;
    }
  }

  if (!explicitOffset && node.mem.base != "loopstart" && node.mem.base != "loopend") {
    offset = implicitOffsets_[node.mem.base];
  }

  size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : 1;
  std::vector<uint32_t> addresses;
  for (size_t lane = 0; lane < lanes; ++lane) {
    addresses.push_back(base + offset + static_cast<uint32_t>(lane * node.mem.sizeBytes));
  }
  if (!explicitOffset && node.mem.base != "loopstart" && node.mem.base != "loopend") {
    implicitOffsets_[node.mem.base] += static_cast<uint32_t>(lanes * node.mem.sizeBytes);
  }
  return addresses;
}

Value Simulator::executeNode(const DFGNode &node) {
  if (isLoadOp(node.op)) {
    if (!predicateAllows(node)) {
      return Value::invalid();
    }
    std::vector<uint32_t> addresses = memoryAddresses(node);
    if (node.simdPacked) {
      std::vector<uint32_t> lanes;
      for (uint32_t address : addresses) {
        lanes.push_back(trace_.initial.load(address, node.mem.sizeBytes).scalar());
      }
      return Value(std::move(lanes));
    }
    return trace_.initial.load(addresses[0], node.mem.sizeBytes);
  }

  if (isStoreOp(node.op)) {
    if (!predicateAllows(node)) {
      return Value::invalid();
    }
    std::vector<uint32_t> addresses = memoryAddresses(node);
    Value data = inputByType(node, "I1");
    if (!data.isValid()) {
      data = firstDataInput(node);
    }
    if (!data.isValid()) {
      return Value::invalid();
    }
    if (node.simdPacked || data.isVector()) {
      size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : data.lanes();
      for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t scalar = data.isVector() ? data.vector()[lane] : data.scalar();
        trace_.initial.store(addresses[lane], node.mem.sizeBytes, Value(scalar));
      }
      return data;
    }
    trace_.initial.store(addresses[0], node.mem.sizeBytes, data);
    return data;
  }

  if (node.hasConst && node.inputs.empty()) {
    return node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
  }

  if (node.op == "CMERGE") {
    if (!predicateAllows(node)) {
      return Value::invalid();
    }
    Value data = firstDataInput(node);
    if (data.isValid()) {
      return data;
    }
    if (node.hasConst) {
      return node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
    }
    return Value::invalid();
  }

  if (node.op == "SELECT") {
    Value data = firstDataInput(node);
    if (data.isValid()) {
      return data;
    }
    return secondDataInput(node);
  }

  Value a = inputByType(node, "I1");
  if (!a.isValid()) {
    a = firstDataInput(node);
  }
  Value b = inputByType(node, "I2");
  if (!b.isValid()) {
    b = secondDataInput(node);
  }
  if (!b.isValid() && node.hasConst) {
    b = node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
  }
  if (!a.isValid() && node.hasConst) {
    a = node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
  }
  if (b.isValid()) {
    return binaryOp(a, b, node.op);
  }
  return unaryOp(a, node.op);
}

void Simulator::storeNodeResult(const DFGNode &node, const Value &value) {
  if (value.isValid()) {
    values_[node.idx] = value;
  }
}

bool Simulator::shouldStop() const {
  auto loopEnd = alloc_.base.find("loopend");
  if (loopEnd != alloc_.base.end() && trace_.initial.getByte(loopEnd->second) != 0) {
    return true;
  }
  return false;
}

SimResult Simulator::compare() const {
  SimResult result;
  for (const auto &entry : trace_.expected) {
    uint8_t actual = trace_.initial.getByte(entry.first);
    if (actual == entry.second) {
      result.matches++;
    } else {
      if (result.mismatches < 16) {
        std::cerr << "Mismatch addr=" << entry.first
                  << " actual=" << static_cast<unsigned>(actual)
                  << " expected=" << static_cast<unsigned>(entry.second) << "\n";
      }
      result.mismatches++;
    }
  }
  return result;
}

SimResult Simulator::run() {
  auto ordered = mapping_.latencyOrder();
  if (ordered.empty()) {
    for (const auto &entry : graph_.nodes()) {
      ordered.push_back(Placement{entry.first, 0, 0, entry.second.asap});
    }
    std::sort(ordered.begin(), ordered.end(), [](const Placement &a, const Placement &b) {
      if (a.latency != b.latency) return a.latency < b.latency;
      return a.node < b.node;
    });
  }

  for (int iter = 0; iter < options_.maxIterations; ++iter) {
    previousValues_ = values_;
    values_.clear();
    for (const auto &placement : ordered) {
      if (graph_.nodes().find(placement.node) == graph_.nodes().end()) {
        continue;
      }
      const DFGNode &node = graph_.node(placement.node);
      Value out = executeNode(node);
      if (std::getenv("ARTIFACT_SIM_DEBUG")) {
        if (node.idx == 0 || node.idx == 1 || node.idx == 6 || node.idx == 8 || node.idx == 11 || node.idx == 12 || node.idx == 13 || node.idx == 17 || node.idx == 20015) {
          std::cerr << "iter=" << iter << " node=" << node.idx << " op=" << node.op << " out=" << out.str() << "\n";
        }
      }
      storeNodeResult(node, out);
    }
    if (shouldStop()) {
      break;
    }
  }
  return compare();
}

} // namespace ArtifactSim
