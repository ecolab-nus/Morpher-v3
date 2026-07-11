#include "ArtifactSim/morpher/NodeSemantics.h"

#include "ArtifactSim/core/Ops.h"

#include <algorithm>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ArtifactSim {

Value inputByType(const DFGNode &node, const OperandSource &src,
                  const std::string &type) {
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    if (node.inputs[i].type == type) {
      return src.inputValue(node, i);
    }
  }
  return Value::invalid();
}

Value firstDataInput(const DFGNode &node, const OperandSource &src) {
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    const std::string &type = node.inputs[i].type;
    if (type == "P" || type == "PS") {
      continue;
    }
    Value value = src.inputValue(node, i);
    if (value.isValid()) {
      return value;
    }
  }
  return Value::invalid();
}

Value secondDataInput(const DFGNode &node, const OperandSource &src) {
  bool seenFirst = false;
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    const std::string &type = node.inputs[i].type;
    if (type == "P" || type == "PS") {
      continue;
    }
    Value value = src.inputValue(node, i);
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

bool predicateAllows(const DFGNode &node, const OperandSource &src) {
  bool sawPredicate = false;
  bool allowed = false;
  for (size_t i = 0; i < node.inputs.size(); ++i) {
    const EdgeRef &edge = node.inputs[i];
    // PS edges carry memory/address sequencing in pruned Morpher DFGs.  They
    // are not boolean guards; treating them as predicates skips valid memory
    // events whenever the producer's data value is zero.
    if (edge.type != "P") {
      continue;
    }
    sawPredicate = true;
    Value value = src.inputValue(node, i);
    bool pred = value.isValid() && value.scalar() != 0;
    if (edge.npb) {
      pred = !pred;
    }
    allowed = allowed || pred;
  }
  return !sawPredicate || allowed;
}

std::vector<int> canonicalOrder(const DFGGraph &graph) {
  // ASAP buckets; Kahn within a bucket so a producer always precedes its
  // same-ASAP consumer. A non-transitive comparator would otherwise leave
  // producer/consumer pairs unordered.
  std::map<int, std::vector<int>> bucketsByAsap;
  for (const auto &entry : graph.nodes()) {
    bucketsByAsap[entry.second.asap].push_back(entry.first);
  }

  std::vector<int> ordered;
  ordered.reserve(graph.nodes().size());
  for (auto &kv : bucketsByAsap) {
    std::vector<int> &bucket = kv.second;
    std::unordered_set<int> bucketSet(bucket.begin(), bucket.end());

    std::unordered_map<int, int> indegree;
    indegree.reserve(bucket.size());
    for (int idx : bucket) {
      const DFGNode &node = graph.node(idx);
      int count = 0;
      for (const auto &in : node.inputs) {
        if (!in.nextIter && bucketSet.count(in.idx)) {
          ++count;
        }
      }
      indegree[idx] = count;
    }

    std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
    for (int idx : bucket) {
      if (indegree[idx] == 0) ready.push(idx);
    }

    size_t emitted = 0;
    while (!ready.empty()) {
      int idx = ready.top();
      ready.pop();
      ordered.push_back(idx);
      ++emitted;
      const DFGNode &node = graph.node(idx);
      for (const auto &out : node.outputs) {
        if (!out.nextIter && bucketSet.count(out.idx)) {
          auto it = indegree.find(out.idx);
          if (it != indegree.end() && --(it->second) == 0) {
            ready.push(out.idx);
          }
        }
      }
    }
    // If a cycle exists inside an ASAP bucket (shouldn't happen for a valid
    // DFG), fall back to idx-order for the remainder so we don't drop nodes.
    if (emitted < bucket.size()) {
      for (int idx : bucket) {
        if (indegree[idx] > 0) {
          ordered.push_back(idx);
        }
      }
    }
  }
  return ordered;
}

namespace {

// A memory access uses the implicit layout when nothing else determines its
// address: no GEP offset, no const, no possible dynamic-offset operand.
// This mirrors the runtime branch structure of memoryAddresses().
bool usesImplicitLayout(const DFGNode &node) {
  if (!isLoadOp(node.op) && !isStoreOp(node.op)) {
    return false;
  }
  if (node.mem.hasGepOffset || node.hasConst) {
    return false;
  }
  if (node.mem.base == "loopstart" || node.mem.base == "loopend") {
    return false;
  }
  for (const EdgeRef &in : node.inputs) {
    if (in.type == "I2") {
      return false; // dynamic offset operand
    }
  }
  if (isLoadOp(node.op)) {
    for (const EdgeRef &in : node.inputs) {
      if (in.type != "P" && in.type != "PS") {
        return false; // load with a data input: dynamic offset
      }
    }
  }
  return true;
}

} // namespace

NodeExecutor::NodeExecutor(const DFGGraph &graph, MemAlloc alloc,
                           TraceImage trace,
                           std::map<int, Value> recurrenceSeeds)
    : graph_(graph),
      alloc_(std::move(alloc)),
      trace_(std::move(trace)),
      initialSnapshot_(trace_.initial),
      recurrenceSeeds_(std::move(recurrenceSeeds)) {
  for (const auto &[idx, seed] : recurrenceSeeds_) {
    auto nodeIt = graph_.nodes().find(idx);
    if (nodeIt == graph_.nodes().end() || !seed.isValid()) {
      throw std::runtime_error(
          "recurrence seed refers to an invalid DFG node/value: " +
          std::to_string(idx));
    }
    bool hasDirectI1Recurrence = false;
    for (const EdgeRef &input : nodeIt->second.inputs) {
      if (input.idx == idx && input.nextIter && input.type == "I1") {
        hasDirectI1Recurrence = true;
        break;
      }
    }
    if (!hasDirectI1Recurrence) {
      throw std::runtime_error(
          "recurrence seed node lacks a direct next-iteration I1 edge: " +
          std::to_string(idx));
    }
  }
  // Static implicit layout: canonical order fixes each node's slot within
  // its base's per-iteration round.
  for (int idx : canonicalOrder(graph_)) {
    const DFGNode &node = graph_.node(idx);
    if (!usesImplicitLayout(node)) {
      continue;
    }
    const size_t lanes =
        (node.simdPacked && !node.simdReduceConsumer)
            ? static_cast<size_t>(node.simdLanes)
            : 1;
    const uint32_t bytes =
        static_cast<uint32_t>(lanes) * static_cast<uint32_t>(node.mem.sizeBytes);
    implicitNodes_.insert(idx);
    // Reduction stores accumulate into one fixed slot (base + 0) across all
    // iterations; they neither occupy a round position nor widen the round.
    if (node.simdReduceConsumer) {
      implicitPrefixBytes_[idx] = 0;
    } else {
      implicitPrefixBytes_[idx] = implicitRoundBytes_[node.mem.base];
      implicitRoundBytes_[node.mem.base] += bytes;
    }
  }
}

std::vector<uint32_t> NodeExecutor::memoryAddresses(
    const DFGNode &node, const OperandSource &operands) {
  auto externalIt = trace_.externalAddresses.find(node.idx);
  if (externalIt != trace_.externalAddresses.end()) {
    size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : 1;
    // VRED-reduced stores collapse all SIMD lanes into one trace entry per
    // CGRA iter (see simd_packer.remap_addr_trace's simd_reduced_store_groups
    // handling), so only 1 cursor step per call regardless of simd_lanes.
    size_t cursorStep = node.simdReduceConsumer ? 1 : lanes;
    size_t &cursor = externalAddressOffsets_[node.idx];
    auto sizeIt = trace_.externalAccessSizes.find(node.idx);
    if (node.simdPacked && !node.simdReduceConsumer &&
        sizeIt != trace_.externalAccessSizes.end() &&
        cursor < externalIt->second.size() && cursor < sizeIt->second.size() &&
        sizeIt->second[cursor] == static_cast<uint32_t>(lanes * node.mem.sizeBytes)) {
      uint32_t vectorBase = externalIt->second[cursor++];
      std::vector<uint32_t> addresses;
      addresses.reserve(lanes);
      for (size_t lane = 0; lane < lanes; ++lane) {
        addresses.push_back(vectorBase + static_cast<uint32_t>(lane * node.mem.sizeBytes));
      }
      return addresses;
    }
    if (cursor + cursorStep <= externalIt->second.size()) {
      // For VRED-reduced stores the host trace records the lane-0 partial
      // result address (e.g. .morpher.vecout), not the CGRA-side accumulator
      // (e.g. morpher.psum.final.init).  Advance the cursor so shouldStop()
      // can detect completion, but fall through to compute the DFG base.
      if (node.simdReduceConsumer) {
        cursor += cursorStep;
      } else {
        std::vector<uint32_t> addresses;
        addresses.reserve(lanes);
        for (size_t lane = 0; lane < lanes; ++lane) {
          addresses.push_back(externalIt->second[cursor++]);
        }
        return addresses;
      }
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
    Value dynamicOffset = inputByType(node, operands, "I2");
    if (!dynamicOffset.isValid() && isLoadOp(node.op)) {
      dynamicOffset = firstDataInput(node, operands);
    }
    if (dynamicOffset.isValid()) {
      explicitOffset = true;
      size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : dynamicOffset.lanes();
      std::vector<uint32_t> addresses;
      for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t laneOffset = dynamicOffset.isVector() ? dynamicOffset.vector()[lane] : dynamicOffset.scalar();
        addresses.push_back(base + laneOffset);
      }
      return addresses;
    }
  }

  if (!explicitOffset && node.mem.base != "loopstart" && node.mem.base != "loopend") {
    if (!implicitNodes_.count(node.idx)) {
      throw std::runtime_error(
          "node " + std::to_string(node.idx) + " op=" + node.op +
          " reached implicit addressing but was not classified for it");
    }
    if (!node.simdReduceConsumer) {
      const uint32_t k = implicitExecCount_[node.idx]++;
      offset = k * implicitRoundBytes_.at(node.mem.base) +
               implicitPrefixBytes_.at(node.idx);
    }
  }

  size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : 1;
  std::vector<uint32_t> addresses;
  for (size_t lane = 0; lane < lanes; ++lane) {
    addresses.push_back(base + offset + static_cast<uint32_t>(lane * node.mem.sizeBytes));
  }
  return addresses;
}

Value NodeExecutor::execute(const DFGNode &node, const OperandSource &operands,
                            const Value *physicalMacAccumulator) {
  if (isLoadOp(node.op)) {
    if (!predicateAllows(node, operands)) {
      return Value::invalid();
    }
    auto valueIt = trace_.externalLoadValues.find(node.idx);
    if (valueIt != trace_.externalLoadValues.end()) {
      size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : 1;
      size_t &cursor = externalAddressOffsets_[node.idx];
      if (cursor + lanes <= valueIt->second.size()) {
        if (node.simdPacked) {
          std::vector<uint32_t> lanesOut;
          lanesOut.reserve(lanes);
          for (size_t lane = 0; lane < lanes; ++lane) {
            lanesOut.push_back(valueIt->second[cursor++]);
          }
          return Value(std::move(lanesOut));
        }
        return Value(valueIt->second[cursor++]);
      }
    }
    std::vector<uint32_t> addresses = memoryAddresses(node, operands);
    if (node.op == "OLOAD") {
      return initialSnapshot_.load(addresses[0], node.mem.sizeBytes);
    }
    if (node.simdPacked) {
      std::vector<uint32_t> lanesOut;
      for (uint32_t address : addresses) {
        lanesOut.push_back(trace_.initial.load(address, node.mem.sizeBytes).scalar());
      }
      return Value(std::move(lanesOut));
    }
    return trace_.initial.load(addresses[0], node.mem.sizeBytes);
  }

  if (isStoreOp(node.op)) {
    if (!predicateAllows(node, operands)) {
      return Value::invalid();
    }
    std::vector<uint32_t> addresses = memoryAddresses(node, operands);
    Value data = inputByType(node, operands, "I1");
    if (!data.isValid()) {
      data = firstDataInput(node, operands);
    }
    size_t lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : 1;
    auto storeIt = trace_.externalStoreValues.find(node.idx);
    if ((node.simdPacked || !data.isValid()) && storeIt != trace_.externalStoreValues.end()) {
      size_t &cursor = externalValueOffsets_[node.idx];
      if (cursor + lanes <= storeIt->second.size()) {
        std::vector<uint32_t> traced;
        traced.reserve(lanes);
        for (size_t lane = 0; lane < lanes; ++lane) {
          traced.push_back(storeIt->second[cursor++]);
        }
        data = Value(std::move(traced));
      }
    }
    if (!data.isValid()) {
      return Value::invalid();
    }
    if (node.simdReduceConsumer && data.isVector()) {
      // The VRED reduces the SIMD lanes; the OSTORE writes that scalar to the
      // partial-sum buffer.  The reduction is per-iteration, not cross-
      // iteration: each iter overwrites the slot.  When the rewritten OSTORE
      // is still simd_packed, broadcast the reduced scalar to every lane slot
      // (mirroring the pruned scalar-reduction OSTORE's lane-wise stores).
      const uint32_t scalar = data.vector()[0];
      for (uint32_t addr : addresses) {
        trace_.initial.store(addr, node.mem.sizeBytes, Value(scalar));
        for (int byte = 0; byte < node.mem.sizeBytes; ++byte) {
          writtenAddresses_.insert(addr + static_cast<uint32_t>(byte));
        }
      }
      return Value(scalar);
    }
    if (node.simdPacked || data.isVector()) {
      lanes = node.simdPacked ? static_cast<size_t>(node.simdLanes) : data.lanes();
      for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t scalar = data.isVector() ? data.vector()[lane] : data.scalar();
        trace_.initial.store(addresses[lane], node.mem.sizeBytes, Value(scalar));
        for (int byte = 0; byte < node.mem.sizeBytes; ++byte) {
          writtenAddresses_.insert(addresses[lane] + static_cast<uint32_t>(byte));
        }
      }
      return data;
    }
    trace_.initial.store(addresses[0], node.mem.sizeBytes, data);
    for (int byte = 0; byte < node.mem.sizeBytes; ++byte) {
      writtenAddresses_.insert(addresses[0] + static_cast<uint32_t>(byte));
    }
    return data;
  }

  if (node.hasConst && node.inputs.empty()
      && node.op != "MAC" && node.op != "VMAC") {
    return node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
  }

  if (node.op == "CMERGE") {
    if (!predicateAllows(node, operands)) {
      return Value::invalid();
    }
    Value data = firstDataInput(node, operands);
    if (data.isValid()) {
      return data;
    }
    if (node.hasConst) {
      return node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
    }
    return Value::invalid();
  }

  if (node.op == "SELECT") {
    for (size_t i = 0; i < node.inputs.size(); ++i) {
      if (!node.inputs[i].nextIter) {
        continue;
      }
      Value value = operands.inputValue(node, i);
      if (value.isValid()) {
        return value;
      }
    }
    Value data = firstDataInput(node, operands);
    if (data.isValid()) {
      return data;
    }
    return secondDataInput(node, operands);
  }

  if (node.op == "VRED") {
    Value data = inputByType(node, operands, "I1");
    if (!data.isValid()) {
      data = firstDataInput(node, operands);
    }
    if (!data.isValid()) {
      return Value::invalid();
    }
    std::vector<uint32_t> lanes;
    if (data.isVector()) {
      lanes = data.vector();
    } else {
      lanes = std::vector<uint32_t>(std::max(node.simdLanes, 1), data.scalar());
    }
    const std::string reduceOp = node.simdReduceOp.empty() ? "ADD" : node.simdReduceOp;
    const uint32_t reduced = reduceLanes(lanes, reduceOp);
    std::vector<uint32_t> out(std::max(node.simdLanes, 1), 0);
    out[0] = reduced;
    return Value(std::move(out));
  }

  Value a = inputByType(node, operands, "I1");
  auto seedIt = recurrenceSeeds_.find(node.idx);
  if (!a.isValid() && seedIt != recurrenceSeeds_.end() &&
      !consumedRecurrenceSeeds_.count(node.idx)) {
    // A mapped self edge has no producer value in the first iteration. Seed
    // exactly that first issue; subsequent missing feedback remains invalid
    // and therefore exposes a routing/schedule error instead of being hidden.
    a = seedIt->second;
    consumedRecurrenceSeeds_.insert(node.idx);
  }
  if (!a.isValid()) {
    a = firstDataInput(node, operands);
  }
  Value b = inputByType(node, operands, "I2");
  if (!b.isValid()) {
    b = secondDataInput(node, operands);
  }
  if (!b.isValid() && node.hasConst) {
    b = node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
  }
  if (!a.isValid() && node.hasConst) {
    a = node.simdPacked ? Value::broadcast(node.constValue, node.simdLanes) : Value(node.constValue);
  }

  // MAC / VMAC: multiply-accumulate with an *implicit* accumulator register
  // that lives inside the PE (the hardware TREG). The third operand is this
  // node's own previous-iteration output. On the first iteration the
  // accumulator is 0 by hardware convention. Mapper sees MAC as a normal
  // 2-input op; the accumulator is transparent. The V-prefixed variant
  // behaves identically per-lane (binaryOp already strips the V).
  if (node.op == "MAC" || node.op == "VMAC") {
    if (!a.isValid() || !b.isValid()) {
      return Value::invalid();
    }
    Value acc;
    if (physicalMacAccumulator != nullptr
        && physicalMacAccumulator->isValid()) {
      acc = *physicalMacAccumulator;
    } else {
      Value prev = physicalMacAccumulator == nullptr
                       ? operands.selfPreviousValue(node)
                       : Value::invalid();
      if (prev.isValid()) {
        acc = prev;
      } else {
        acc = node.simdPacked
                  ? Value::broadcast(0u, node.simdLanes)
                  : Value(0u);
      }
    }
    Value product = binaryOp(a, b, "MUL");
    return binaryOp(product, acc, "ADD");
  }

  if (b.isValid()) {
    return binaryOp(a, b, node.op);
  }
  return unaryOp(a, node.op);
}

bool NodeExecutor::shouldStop() const {
  auto loopEnd = alloc_.base.find("loopend");
  if (loopEnd != alloc_.base.end() && trace_.initial.getByte(loopEnd->second) != 0) {
    return true;
  }

  // When agi_iter externalizes loop-control stores, loopend is never written.
  // Stop once all traced node address streams are consumed; otherwise the
  // simulator falls back to synthetic memory addresses and overwrites valid
  // outputs after the last traced iteration.
  bool hasExternalTraces = false;
  for (const auto &[idx, node] : graph_.nodes()) {
    auto addrIt = trace_.externalAddresses.find(idx);
    if (addrIt == trace_.externalAddresses.end()) continue;
    hasExternalTraces = true;
    size_t cursor = externalAddressOffsets_.count(idx)
                        ? externalAddressOffsets_.at(idx) : 0;
    if (cursor < addrIt->second.size()) {
      return false;  // still have traced accesses to process
    }
  }
  return hasExternalTraces;  // true only when all traced accesses are done
}

SimResult NodeExecutor::compare() const {
  // Identify which allocation bases have at least one STORE node in the DFG.
  // When agi_iter externalizes OSTORE nodes, their bases have no stores; expected
  // bytes in those ranges should be skipped rather than reported as mismatches.
  std::unordered_set<std::string> storedBases;
  for (const auto &[idx, node] : graph_.nodes()) {
    if (isStoreOp(node.op) && !node.mem.base.empty()) {
      storedBases.insert(node.mem.base);
    }
  }

  // Build a sorted list of (base_address, name) for range lookup.
  // Each address belongs to the allocation whose base is the largest value <= it.
  std::vector<std::pair<uint32_t, std::string>> sortedBases;
  sortedBases.reserve(alloc_.base.size());
  for (const auto &kv : alloc_.base) {
    sortedBases.emplace_back(kv.second, kv.first);  // (addr, name)
  }
  std::sort(sortedBases.begin(), sortedBases.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  bool allBasesStored = (storedBases.size() >= alloc_.base.size());
  // When the DFG has no <BasePointerName> on any node (e.g., synthesized
  // systolic templates), `storedBases` is empty. In that case the
  // base-coverage filter would skip every expected address. Fall back to
  // the per-address `writtenAddresses_` filter instead.
  if (storedBases.empty() && !writtenAddresses_.empty()) {
    allBasesStored = true;
  }

  auto isStoreCovered = [&](uint32_t addr) -> bool {
    if (allBasesStored) return true;
    // Find the allocation this address belongs to.
    auto it = std::upper_bound(
        sortedBases.begin(), sortedBases.end(), addr,
        [](uint32_t val, const std::pair<uint32_t, std::string> &elem) {
          return val < elem.first;
        });
    if (it == sortedBases.begin()) return true; // below all bases, include conservatively
    --it;
    return storedBases.count(it->second) > 0;
  };

  SimResult result;
  auto compareAddress = [&](uint32_t address, uint8_t expected) {
    if (!isStoreCovered(address)) {
      return;
    }
    if (storedBases.empty() && !writtenAddresses_.count(address)) {
      return;
    }
    if (!storedBases.empty() && !writtenAddresses_.count(address)) {
      if (result.mismatches < 16) {
        std::cerr << "Mismatch addr=" << address
                  << " was not written; expected=" << static_cast<unsigned>(expected)
                  << "\n";
      }
      result.mismatches++;
      return;
    }
    uint8_t actual = trace_.initial.getByte(address);
    if (actual == expected) {
      result.matches++;
    } else {
      if (result.mismatches < 16) {
        std::cerr << "Mismatch addr=" << address
                  << " actual=" << static_cast<unsigned>(actual)
                  << " expected=" << static_cast<unsigned>(expected) << "\n";
      }
      result.mismatches++;
    }
  };

  if (!trace_.loopStoreExpectedAddresses.empty()) {
    for (uint32_t address : trace_.loopStoreExpectedAddresses) {
      auto expectedIt = trace_.expected.find(address);
      if (expectedIt == trace_.expected.end()) continue;
      compareAddress(address, expectedIt->second);
    }
  } else {
    for (const auto &entry : trace_.expected) {
      compareAddress(entry.first, entry.second);
    }
  }
  return result;
}

} // namespace ArtifactSim
