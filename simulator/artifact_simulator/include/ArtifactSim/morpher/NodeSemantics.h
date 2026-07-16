#pragma once

#include "ArtifactSim/core/Value.h"
#include "ArtifactSim/morpher/DFG.h"
#include "ArtifactSim/morpher/Memory.h"
#include "ArtifactSim/morpher/Trace.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ArtifactSim {

struct SimResult {
  int matches = 0;
  int mismatches = 0;
};

// How a node's operands are delivered. The functional simulator resolves
// them through DFG edges and per-iteration value maps; the cycle-accurate
// engine resolves them from the FU's operand ports. Everything downstream
// of operand delivery (op semantics, predication, trace-driven memory) is
// shared, so the two engines cannot diverge on what an op *means*.
class OperandSource {
public:
  virtual ~OperandSource() = default;
  // Value of node.inputs[index]'s producer as observable by `node` now.
  virtual Value inputValue(const DFGNode &node, size_t index) const = 0;
  // The node's own previous-iteration output (MAC recurrence fallback when
  // no physical accumulator is supplied). May return invalid.
  virtual Value selfPreviousValue(const DFGNode &node) const = 0;
  // Launch-preloaded regular-register operand omitted from steady-state DFG.
  virtual Value stationaryValue(const DFGNode &node,
                                const std::string &type) const = 0;
};

// Derived operand helpers (Morpher DFG edge-type conventions).
Value inputByType(const DFGNode &node, const OperandSource &src, const std::string &type);
Value firstDataInput(const DFGNode &node, const OperandSource &src);
Value secondDataInput(const DFGNode &node, const OperandSource &src);
bool predicateAllows(const DFGNode &node, const OperandSource &src);

// Canonical node order: ASAP buckets, Kahn within a bucket. This is the
// functional simulator's execution order and the reference order for
// implicit memory layout (see NodeExecutor).
std::vector<int> canonicalOrder(const DFGGraph &graph);

// Executes DFG nodes against the trace-driven memory model and validates the
// final memory image. Owns all Morpher-trace-specific policy: external
// address/value cursors, implicit per-base offsets, GEP offsets, SIMD packing,
// VRED-reduced stores, loopend sentinels, and store-coverage filtering in
// compare(). One instance per phase; the TraceImage carries across phases.
class NodeExecutor {
public:
  NodeExecutor(const DFGGraph &graph, MemAlloc alloc, TraceImage trace,
               std::map<int, Value> recurrenceSeeds = {});

  // `physicalMacAccumulator`: per-PE TREG state for MAC/VMAC (cycle engine
  // and multi-phase runs); nullptr = use the node's own recurrence.
  Value execute(const DFGNode &node, const OperandSource &operands,
                const Value *physicalMacAccumulator = nullptr);

  bool shouldStop() const;
  SimResult compare() const;
  const TraceImage &traceImage() const { return trace_; }

private:
  std::vector<uint32_t> memoryAddresses(const DFGNode &node,
                                        const OperandSource &operands);

  const DFGGraph &graph_;
  MemAlloc alloc_;
  TraceImage trace_;
  MemoryImage initialSnapshot_;
  // Implicit memory layout for pruned DFGs whose address computation was
  // externalized (agi_iter): accesses without GEP/const/dynamic offsets are
  // laid out per base in canonical node order, one "round" per iteration:
  //   address = base + k * roundBytes(base) + prefixBytes(node)
  // where k counts the node's own executions. Unlike a shared running
  // cursor, this is independent of the engine's execution order, so the
  // functional and cycle-accurate engines produce identical layouts.
  std::set<int> implicitNodes_;
  std::map<int, uint32_t> implicitPrefixBytes_;
  std::map<std::string, uint32_t> implicitRoundBytes_;
  std::map<int, uint32_t> implicitExecCount_;
  std::map<int, size_t> externalAddressOffsets_;
  std::map<int, size_t> externalValueOffsets_;
  std::set<uint32_t> writtenAddresses_;
  std::map<int, Value> recurrenceSeeds_;
  std::set<int> consumedRecurrenceSeeds_;
};

} // namespace ArtifactSim
