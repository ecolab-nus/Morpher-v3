#include "ArtifactSim/morpher/CycleSimulator.h"

#include <iostream>

namespace ArtifactSim {

namespace {

// Operands come from the FU's ports, delivered by the routed fabric.
// The port a DFG edge maps to is determined by its edge type; timing is
// physical, so there is no previous-iteration map (a nextIter edge's value
// simply arrives on the port when the fabric delivers it).
class PortOperandSource : public OperandSource {
public:
  PortOperandSource(Value i1, Value i2, Value p, int node,
                    const std::map<std::pair<int, int>, Value> &stationary)
      : i1_(std::move(i1)), i2_(std::move(i2)), p_(std::move(p)),
        node_(node), stationary_(stationary) {}

  Value inputValue(const DFGNode &node, size_t index) const override {
    if (index >= node.inputs.size()) {
      if (node.hasConst) {
        return node.simdPacked
                   ? Value::broadcast(node.constValue, node.simdLanes)
                   : Value(node.constValue);
      }
      return Value::invalid();
    }
    const std::string &type = node.inputs[index].type;
    if (type == "I1") return i1_;
    if (type == "I2") return i2_;
    if (type == "P") return p_;
    // PS edges carry sequencing, not data; nothing rides a port for them.
    return Value::invalid();
  }

  Value selfPreviousValue(const DFGNode &) const override {
    // The cycle engine always supplies the physical MAC accumulator.
    return Value::invalid();
  }

  Value stationaryValue(const DFGNode &, const std::string &type) const override {
    const int operand = type == "I1" ? 1 : type == "I2" ? 2 : -1;
    auto it = stationary_.find({node_, operand});
    return it != stationary_.end() ? it->second : Value::invalid();
  }

private:
  Value i1_, i2_, p_;
  int node_;
  const std::map<std::pair<int, int>, Value> &stationary_;
};

} // namespace

CycleSimulator::CycleSimulator(const Fabric &fabric, const ConfigMem &config,
                               const DFGGraph &graph, MemAlloc alloc,
                               TraceImage trace, int maxIterations,
                               BoundaryContract boundary)
    : fabric_(fabric),
      config_(config),
      graph_(graph),
      boundary_(std::move(boundary)),
      stationaryOperands_(boundary_.stationaryOperands(trace)),
      exec_(graph_, std::move(alloc), trace,
            boundary_.recurrenceSeeds(trace)),
      maxIterations_(maxIterations) {}

SimResult CycleSimulator::run(std::map<std::pair<int, int>, Value> &macState,
                              bool compareAfter) {
  if (boundary_.tregResetOnLaunch()) {
    // `macState` is shared across phases/invocations to model physical PE
    // state. Clearing it here makes reset an action at every loop launch,
    // rather than relying on construction-time zero initialization.
    applyLaunchStateActions(boundary_, fabric_.tregResetOnLaunch, macState);
  }
  EngineHooks hooks;
  hooks.execute = [&](const FUInstr &instr, const Value &i1, const Value &i2,
                      const Value &p, int /*cycle*/) -> Value {
    const DFGNode &node = graph_.node(instr.node);
    const FabricPE &pe = fabric_.pes[static_cast<size_t>(
        fabric_.portPE[static_cast<size_t>(instr.i1Port)])];
    PortOperandSource operands(i1, i2, p, node.idx, stationaryOperands_);
    const bool isMac = node.op == "MAC" || node.op == "VMAC";
    Value *accumulator = isMac ? &macState[{pe.x, pe.y}] : nullptr;
    Value out = exec_.execute(node, operands, accumulator);
    if (isMac && out.isValid()) {
      macState[{pe.x, pe.y}] = out;
    }
    // Keep the latest issue even when invalid. Retaining an older valid value
    // would let a broken recurrence route in a later iteration pass boundary
    // validation accidentally.
    finalValues_[node.idx] = out;
    return out;
  };
  hooks.shouldStop = [&]() { return exec_.shouldStop(); };

  Engine engine(fabric_, config_, hooks);
  engine.run(maxIterations_ * config_.II);

  SimResult result = compareAfter ? exec_.compare() : SimResult{};
  if (!boundary_.empty()) {
    BoundaryResult boundaryResult =
        validateBoundaryContract(boundary_, finalValues_, exec_.traceImage());
    std::cout << "Boundary Validation: Matches::" << boundaryResult.matches
              << ", Mismatches::" << boundaryResult.mismatches << "\n";
    result.matches += boundaryResult.matches;
    result.mismatches += boundaryResult.mismatches;
  }
  return result;
}

} // namespace ArtifactSim
