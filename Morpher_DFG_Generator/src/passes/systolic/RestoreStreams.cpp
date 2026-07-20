// RestoreSystolicMemoryStreamsPass: reattach per-load input-stream and per-MAC
// coordinate metadata after LLVM cleanup (GVN drops unknown instruction
// metadata). Split out of the original SystolicCandidatePass.cpp.

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "SystolicInternal.h"

#include <functional>
#include <string>

using namespace llvm;
using namespace morpher_systolic;

namespace {

// Stream/coordinate metadata names used only by this TU; keep file-local.
constexpr StringLiteral SystolicMemStreamMetadata =
    "morpher.systolic.mem_stream";
constexpr StringLiteral SystolicMemStreamIndexMetadata =
    "morpher.systolic.mem_stream_index";
constexpr StringLiteral SystolicMacCoordMetadata =
    "morpher.systolic.mac_coord";
constexpr StringLiteral SystolicDataflowMetadata =
    "morpher.systolic.dataflow";
constexpr StringLiteral SystolicStationaryWeightMetadata =
    "morpher.systolic.stationary_weight";

} // namespace

namespace morpher_systolic {

PreservedAnalyses
RestoreSystolicMemoryStreamsPass::run(Function &function,
                                     FunctionAnalysisManager &analysisManager) {
    Attribute bindingsAttr = function.getFnAttribute(SystolicMemStreamBindingsAttr);
    Attribute dataflowAttr = function.getFnAttribute(SystolicDataflowAttr);
    if (!bindingsAttr.isStringAttribute())
      return PreservedAnalyses::all();
    if (!dataflowAttr.isStringAttribute())
      report_fatal_error("systolic transform lost its selected dataflow");
    const StringRef selectedDataflow = dataflowAttr.getValueAsString();

    LoopInfo &loopInfo = analysisManager.getResult<LoopAnalysis>(function);
    Loop *selectedReduction = nullptr;
    for (Loop *loop : loopInfo.getLoopsInPreorder()) {
      if (!findLoopProperty(loop, SystolicReductionMetadata))
        continue;
      if (selectedReduction)
        report_fatal_error(
            "systolic transform produced multiple rolled reduction loops");
      selectedReduction = loop;
    }
    if (!selectedReduction)
      report_fatal_error(
          "systolic transform lost its rolled reduction loop");
    Loop *selectedRegion = selectedReduction;
    while (selectedRegion->getParentLoop())
      selectedRegion = selectedRegion->getParentLoop();
    DenseSet<const BasicBlock *> selectedBlocks;
    for (BasicBlock *block : selectedRegion->blocks())
      selectedBlocks.insert(block);
    // Exact WS fully unrolls the reduction and hoists its stationary loads to
    // the surviving streamed loop's preheader.  That preheader is still part
    // of the selected region; unrelated sibling nests remain excluded.
    if (BasicBlock *preheader = selectedRegion->getLoopPreheader())
      selectedBlocks.insert(preheader);

    auto baseKey = [](Value *base) -> std::string {
      if (auto *argument = dyn_cast<Argument>(base))
        return "a" + std::to_string(argument->getArgNo());
      if (auto *global = dyn_cast<GlobalValue>(base))
        return "g" + std::to_string(GlobalValue::getGUID(global->getName()));
      if (auto *alloca = dyn_cast<AllocaInst>(base))
        return alloca->getName().empty()
                   ? ""
                   : "l" + std::to_string(
                         GlobalValue::getGUID(alloca->getName()));
      return "";
    };
    SmallVector<std::pair<std::string, std::string>, 4> bindings;
    SmallVector<StringRef, 4> encodedBindings;
    bindingsAttr.getValueAsString().split(encodedBindings, ',', -1, false);
    for (StringRef encoded : encodedBindings) {
      auto split = encoded.split(':');
      if (!split.first.empty() && !split.second.empty())
        bindings.emplace_back(split.first.str(), split.second.str());
    }

    unsigned taggedLoads = 0;
    unsigned matchingLoadsOutsideRegion = 0;
    SmallVector<LoadInst *, 8> rowLoads;
    SmallVector<LoadInst *, 8> columnLoads;
    for (BasicBlock &block : function) {
      for (Instruction &instruction : block) {
        auto *load = dyn_cast<LoadInst>(&instruction);
        if (!load)
          continue;
        Value *base = resolveBasePointer(load->getPointerOperand());
        const std::string key = baseKey(base);
        for (const auto &binding : bindings) {
          if (binding.second != key)
            continue;
          if (!selectedBlocks.count(&block)) {
            ++matchingLoadsOutsideRegion;
            break;
          }
          LLVMContext &context = instruction.getContext();
          instruction.setMetadata(
              SystolicMemStreamMetadata,
              MDNode::get(context, MDString::get(context, binding.first)));
          instruction.setMetadata(
              SystolicDataflowMetadata,
              MDNode::get(context, MDString::get(context, selectedDataflow)));
          if (binding.first == "row")
            rowLoads.push_back(load);
          else if (binding.first == "column")
            columnLoads.push_back(load);
          ++taggedLoads;
          break;
        }
      }
    }
    function.removeFnAttr(SystolicMemStreamBindingsAttr);
    function.removeFnAttr(SystolicDataflowAttr);
    if (taggedLoads == 0)
      report_fatal_error(
          Twine("systolic transform lost every classified input memory stream; matching loads outside selected region=") +
          Twine(matchingLoadsOutsideRegion));
    const bool weightStationary = selectedDataflow == "weight_stationary";
    const unsigned expectedRowLoads = MorpherSystolicFactor0;
    const unsigned expectedColumnLoads = weightStationary
        ? MorpherSystolicFactor0 * MorpherSystolicFactor1
        : MorpherSystolicFactor1;
    if (rowLoads.size() != expectedRowLoads ||
        columnLoads.size() != expectedColumnLoads)
      report_fatal_error(
          Twine("systolic transform input stream count does not match jam factors: row=") +
          Twine(rowLoads.size()) + "/" + Twine(expectedRowLoads) +
          ", column=" + Twine(columnLoads.size()) + "/" +
          Twine(expectedColumnLoads) + ", matching loads outside selected region=" +
          Twine(matchingLoadsOutsideRegion) + ", bindings=" +
          bindingsAttr.getValueAsString());

    DenseMap<LoadInst *, unsigned> rowIndex;
    DenseMap<LoadInst *, unsigned> columnIndex;
    auto attachIndices = [](ArrayRef<LoadInst *> loads,
                            DenseMap<LoadInst *, unsigned> &indices) {
      for (unsigned index = 0; index < loads.size(); ++index) {
        LoadInst *load = loads[index];
        indices[load] = index;
        LLVMContext &context = load->getContext();
        load->setMetadata(
            SystolicMemStreamIndexMetadata,
            MDNode::get(context, ConstantAsMetadata::get(
                                     ConstantInt::get(Type::getInt32Ty(context),
                                                      index))));
      }
    };
    attachIndices(rowLoads, rowIndex);
    if (!weightStationary)
      attachIndices(columnLoads, columnIndex);

    SmallPtrSet<LoadInst *, 16> rowLoadSet;
    SmallPtrSet<LoadInst *, 16> columnLoadSet;
    rowLoadSet.insert(rowLoads.begin(), rowLoads.end());
    columnLoadSet.insert(columnLoads.begin(), columnLoads.end());
    auto classifiedLoad = [](Value *value,
                             const SmallPtrSetImpl<LoadInst *> &members)
        -> LoadInst * {
      std::function<LoadInst *(Value *)> find = [&](Value *candidate)
          -> LoadInst * {
        while (auto *cast = dyn_cast<CastInst>(candidate))
          candidate = cast->getOperand(0);
        if (auto *load = dyn_cast<LoadInst>(candidate))
          return members.count(load) ? load : nullptr;
        auto *binary = dyn_cast<BinaryOperator>(candidate);
        if (!binary || !isMulShape(binary->getOpcode()))
          return nullptr;
        LoadInst *lhs = find(binary->getOperand(0));
        LoadInst *rhs = find(binary->getOperand(1));
        return lhs && rhs ? nullptr : lhs ? lhs : rhs;
      };
      return find(value);
    };
    unsigned taggedMacs = 0;
    for (BasicBlock &block : function) {
      if (!selectedBlocks.count(&block))
        continue;
      for (Instruction &instruction : block) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (weightStationary) {
          auto *product = dyn_cast<BinaryOperator>(&instruction);
          if (!product || !isMulShape(product->getOpcode()))
            continue;
          lhs = product->getOperand(0);
          rhs = product->getOperand(1);
        } else if (auto *intrinsic = dyn_cast<IntrinsicInst>(&instruction)) {
          Intrinsic::ID id = intrinsic->getIntrinsicID();
          if (id != Intrinsic::fmuladd && id != Intrinsic::fma)
            continue;
          lhs = intrinsic->getOperand(0);
          rhs = intrinsic->getOperand(1);
        } else {
          auto *binary = dyn_cast<BinaryOperator>(&instruction);
          if (!binary)
            continue;
          if (!isAddShape(binary->getOpcode())) {
            continue;
          } else {
            BinaryOperator *product = nullptr;
            for (unsigned operand = 0; operand < 2; ++operand) {
              auto *candidate =
                  dyn_cast<BinaryOperator>(binary->getOperand(operand));
              if (candidate && isMulShape(candidate->getOpcode())) {
                product = candidate;
                break;
              }
            }
            if (!product)
              continue;
            lhs = product->getOperand(0);
            rhs = product->getOperand(1);
          }
        }

        LoadInst *row = classifiedLoad(lhs, rowLoadSet);
        if (!row)
          row = classifiedLoad(rhs, rowLoadSet);
        LoadInst *column = classifiedLoad(lhs, columnLoadSet);
        if (!column)
          column = classifiedLoad(rhs, columnLoadSet);
        if (!row || !column)
          continue;
        if (weightStationary && !columnIndex.count(column)) {
          const unsigned rowCoordinate = rowIndex[row];
          unsigned nextColumn = 0;
          for (const auto &entry : columnIndex)
            if (entry.second / MorpherSystolicFactor1 == rowCoordinate)
              ++nextColumn;
          if (nextColumn >= MorpherSystolicFactor1)
            report_fatal_error(
                "weight-stationary transform produced too many weights for one reduction row");
          columnIndex[column] =
              rowCoordinate * MorpherSystolicFactor1 + nextColumn;
          LLVMContext &weightContext = column->getContext();
          Metadata *weightCoordinates[] = {
              ConstantAsMetadata::get(ConstantInt::get(
                  Type::getInt32Ty(weightContext), rowCoordinate)),
              ConstantAsMetadata::get(ConstantInt::get(
                  Type::getInt32Ty(weightContext), nextColumn))};
          column->setMetadata(
              SystolicStationaryWeightMetadata,
              MDNode::get(weightContext, weightCoordinates));
        }
        LLVMContext &context = instruction.getContext();
        const unsigned columnCoordinate = weightStationary
            ? columnIndex[column] % MorpherSystolicFactor1
            : columnIndex[column];
        Metadata *coordinates[] = {
            ConstantAsMetadata::get(ConstantInt::get(
                Type::getInt32Ty(context), rowIndex[row])),
            ConstantAsMetadata::get(ConstantInt::get(
                Type::getInt32Ty(context), columnCoordinate))};
        instruction.setMetadata(SystolicMacCoordMetadata,
                                MDNode::get(context, coordinates));
        instruction.setMetadata(
            SystolicDataflowMetadata,
            MDNode::get(context, MDString::get(context, selectedDataflow)));
        ++taggedMacs;
      }
    }
    const unsigned expectedMacs =
        MorpherSystolicFactor0 * MorpherSystolicFactor1;
    if (taggedMacs != expectedMacs)
      report_fatal_error(
          Twine("systolic transform MAC coordinate count does not match jam tile: found ") +
          Twine(taggedMacs) + ", expected " + Twine(expectedMacs));
    errs() << "Morpher systolic unroll-and-jam restored " << taggedLoads
           << " indexed input stream tag(s) and " << taggedMacs
           << " MAC coordinate(s) in " << function.getName() << "\n";
    return PreservedAnalyses::none();
}

} // namespace morpher_systolic
