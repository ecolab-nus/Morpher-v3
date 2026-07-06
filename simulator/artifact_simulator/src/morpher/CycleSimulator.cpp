#include "ArtifactSim/morpher/CycleSimulator.h"

namespace ArtifactSim {

namespace {

// Operands come from the FU's ports, delivered by the routed fabric.
// The port a DFG edge maps to is determined by its edge type; timing is
// physical, so there is no previous-iteration map (a nextIter edge's value
// simply arrives on the port when the fabric delivers it).
class PortOperandSource : public OperandSource {
public:
  PortOperandSource(Value i1, Value i2, Value p)
      : i1_(std::move(i1)), i2_(std::move(i2)), p_(std::move(p)) {}

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

private:
  Value i1_, i2_, p_;
};

} // namespace

CycleSimulator::CycleSimulator(const Fabric &fabric, const ConfigMem &config,
                               const DFGGraph &graph, MemAlloc alloc,
                               TraceImage trace, int maxIterations)
    : fabric_(fabric),
      config_(config),
      graph_(graph),
      exec_(graph_, std::move(alloc), std::move(trace)),
      maxIterations_(maxIterations) {}

SimResult CycleSimulator::run(std::map<std::pair<int, int>, Value> &macState,
                              bool compareAfter) {
  EngineHooks hooks;
  hooks.execute = [&](const FUInstr &instr, const Value &i1, const Value &i2,
                      const Value &p, int /*cycle*/) -> Value {
    const DFGNode &node = graph_.node(instr.node);
    const FabricPE &pe = fabric_.pes[static_cast<size_t>(
        fabric_.portPE[static_cast<size_t>(instr.i1Port)])];
    PortOperandSource operands(i1, i2, p);
    const bool isMac = node.op == "MAC" || node.op == "VMAC";
    Value *accumulator = isMac ? &macState[{pe.x, pe.y}] : nullptr;
    Value out = exec_.execute(node, operands, accumulator);
    if (isMac && out.isValid()) {
      macState[{pe.x, pe.y}] = out;
    }
    return out;
  };
  hooks.shouldStop = [&]() { return exec_.shouldStop(); };

  Engine engine(fabric_, config_, hooks);
  engine.run(maxIterations_ * config_.II);

  return compareAfter ? exec_.compare() : SimResult{};
}

} // namespace ArtifactSim
