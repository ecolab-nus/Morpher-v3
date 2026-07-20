#pragma once

// Shared internals for the Morpher systolic candidate/transform pass family.
// The original SystolicCandidatePass.cpp wrapped every helper, struct, cl::opt,
// and pass in a single anonymous namespace; splitting it across translation
// units requires the items used by more than one .cpp to gain external linkage
// under a named namespace. Detector-only helpers and structs stay file-local in
// DetectPass.cpp.

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/CommandLine.h"
#include <cstdint>
#include <optional>
#include <string>

namespace morpher_systolic {

using namespace llvm;

// ----------------------------------------------------------------------------
// Shared IR metadata / attribute names (defined once here; internal linkage per
// TU is fine for these constexpr constants). Detector-only stream metadata
// names remain file-local in RestoreStreams.cpp.
// ----------------------------------------------------------------------------

inline constexpr StringLiteral SystolicRecipeMetadata =
    "morpher.systolic.transform.recipe.v1";
inline constexpr StringLiteral SystolicReductionMetadata =
    "morpher.systolic.rolled_reduction.v1";
inline constexpr StringLiteral SystolicMemStreamBindingsAttr =
    "morpher-systolic-mem-stream-bindings";
inline constexpr StringLiteral SystolicDataflowAttr =
    "morpher-systolic-selected-dataflow";
inline constexpr StringLiteral SystolicStrippedMarkerAttr =
    "morpher-systolic-stripped-map-marker";

// ----------------------------------------------------------------------------
// cl::opts used by more than one TU. Each has exactly one definition:
//   MorpherSystolicCandidateIndex, MorpherSystolicDataflow -> DetectPass.cpp
//   MorpherSystolicTransformFunction                       -> MarkerPasses.cpp
//   MorpherSystolicFactor0, MorpherSystolicFactor1         -> ConfigureUnrollJam.cpp
// Single-TU opts (MorpherSystolicReport, MorpherSystolicFPNondeterminism) stay
// file-local static in their owning .cpp.
// ----------------------------------------------------------------------------

extern cl::opt<std::string> MorpherSystolicTransformFunction;
extern cl::opt<unsigned> MorpherSystolicCandidateIndex;
extern cl::opt<unsigned> MorpherSystolicFactor0;
extern cl::opt<unsigned> MorpherSystolicFactor1;
extern cl::opt<std::string> MorpherSystolicDataflow;

// ----------------------------------------------------------------------------
// Shared data structures
// ----------------------------------------------------------------------------

struct TransformDimension {
  unsigned level = 0;
  uint64_t tripCount = 0;
  SmallVector<uint64_t, 8> exactDivisors;
};

struct TransformRecipe {
  static constexpr unsigned SchemaVersion = 1;

  bool supported = false;
  std::string reason;
  SmallVector<TransformDimension, 2> parallelDimensions;
  unsigned reductionLevel = 0;
  uint64_t reductionTripCount = 0;
};

struct ParsedTransformRecipe {
  unsigned candidateIndex = 0;
  SmallVector<unsigned, 2> dimensionLevels;
  SmallVector<uint64_t, 2> dimensionTripCounts;
  unsigned reductionLevel = 0;
  uint64_t reductionTripCount = 0;
};

// ----------------------------------------------------------------------------
// Opcode shape predicates
// ----------------------------------------------------------------------------

inline bool isAddShape(unsigned op) { return op == Instruction::Add || op == Instruction::FAdd; }
inline bool isMulShape(unsigned op) { return op == Instruction::Mul || op == Instruction::FMul; }

inline Value *resolveBasePointer(Value *PtrOp) {
  Value *cur = PtrOp;
  while (true) {
    if (auto *GEP = dyn_cast<GEPOperator>(cur)) {
      cur = GEP->getPointerOperand();
      continue;
    }
    if (auto *BC = dyn_cast<BitCastOperator>(cur)) {
      cur = BC->getOperand(0);
      continue;
    }
    break;
  }
  return cur;
}

inline std::optional<uint64_t> constantTripCount(Loop *L, ScalarEvolution &SE) {
  const SCEV *backedge = SE.getBackedgeTakenCount(L);
  if (auto *C = dyn_cast<SCEVConstant>(backedge)) {
    const APInt &v = C->getAPInt();
    if (!v.isNegative())
      return v.getZExtValue() + 1;
  }
  unsigned small = SE.getSmallConstantTripCount(L);
  if (small > 0)
    return small;
  return std::nullopt;
}

inline MDNode *findLoopProperty(const Loop *loop, StringRef name) {
  MDNode *loopID = loop ? loop->getLoopID() : nullptr;
  if (!loopID)
    return nullptr;
  for (unsigned operandIndex = 1; operandIndex < loopID->getNumOperands();
       ++operandIndex) {
    auto *property = dyn_cast_or_null<MDNode>(loopID->getOperand(operandIndex));
    if (!property || property->getNumOperands() == 0)
      continue;
    auto *propertyName =
        dyn_cast_or_null<MDString>(property->getOperand(0));
    if (propertyName && propertyName->getString() == name)
      return property;
  }
  return nullptr;
}

inline std::optional<uint64_t> metadataUnsigned(MDNode *node, unsigned operandIndex) {
  if (!node || operandIndex >= node->getNumOperands())
    return std::nullopt;
  auto *constant =
      dyn_cast_or_null<ConstantAsMetadata>(node->getOperand(operandIndex));
  auto *integer = constant ? dyn_cast<ConstantInt>(constant->getValue()) : nullptr;
  if (!integer || integer->isNegative())
    return std::nullopt;
  return integer->getZExtValue();
}

inline std::optional<ParsedTransformRecipe> parseTransformRecipe(const Loop *loop) {
  MDNode *property = findLoopProperty(loop, SystolicRecipeMetadata);
  if (!property ||
      (property->getNumOperands() != 7 && property->getNumOperands() != 9))
    return std::nullopt;
  auto schema = metadataUnsigned(property, 1);
  auto candidate = metadataUnsigned(property, 2);
  if (!schema || *schema != TransformRecipe::SchemaVersion || !candidate)
    return std::nullopt;

  ParsedTransformRecipe recipe;
  recipe.candidateIndex = static_cast<unsigned>(*candidate);
  const unsigned dimensionCount = (property->getNumOperands() - 5) / 2;
  for (unsigned dimension = 0; dimension < dimensionCount; ++dimension) {
    auto level = metadataUnsigned(property, 3 + 2 * dimension);
    auto tripCount = metadataUnsigned(property, 4 + 2 * dimension);
    if (!level || !tripCount)
      return std::nullopt;
    recipe.dimensionLevels.push_back(static_cast<unsigned>(*level));
    recipe.dimensionTripCounts.push_back(*tripCount);
  }
  auto reductionLevel = metadataUnsigned(property, 3 + 2 * dimensionCount);
  auto reductionTripCount = metadataUnsigned(property, 4 + 2 * dimensionCount);
  if (!reductionLevel || !reductionTripCount)
    return std::nullopt;
  recipe.reductionLevel = static_cast<unsigned>(*reductionLevel);
  recipe.reductionTripCount = *reductionTripCount;
  return recipe;
}

inline SmallVector<Loop *, 4> uniqueLoopChain(Loop *outer) {
  SmallVector<Loop *, 4> levels;
  for (Loop *loop = outer; loop != nullptr;) {
    levels.push_back(loop);
    ArrayRef<Loop *> subLoops = loop->getSubLoops();
    if (subLoops.empty())
      break;
    if (subLoops.size() != 1)
      return {};
    loop = subLoops.front();
  }
  return levels;
}

// ----------------------------------------------------------------------------
// Pass declarations. Each pass' run() is defined out-of-line in exactly one
// .cpp. Complete declarations live here because createMorpherSystolicUnrollJam
// Pipeline() (ConfigureUnrollJam.cpp) constructs passes defined in DetectPass,
// MarkerPasses, CanonicalizeOutput, and RestoreStreams.
// ----------------------------------------------------------------------------

struct SystolicCandidatePass : public PassInfoMixin<SystolicCandidatePass> {
  explicit SystolicCandidatePass(bool emitJSONReport = true)
      : emitJSONReport(emitJSONReport) {}

  bool emitJSONReport;

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

struct StripSystolicMapMarkerPass
    : public PassInfoMixin<StripSystolicMapMarkerPass> {
  PreservedAnalyses run(Function &function,
                        FunctionAnalysisManager &analysisManager);
};

struct RestoreSystolicMapMarkerPass
    : public PassInfoMixin<RestoreSystolicMapMarkerPass> {
  PreservedAnalyses run(Function &function,
                        FunctionAnalysisManager &analysisManager);
};

struct CanonicalizeSystolicOutputPass
    : public PassInfoMixin<CanonicalizeSystolicOutputPass> {
  PreservedAnalyses run(Function &function,
                        FunctionAnalysisManager &analysisManager);
};

struct ConfigureSystolicUnrollJamPass
    : public PassInfoMixin<ConfigureSystolicUnrollJamPass> {
  PreservedAnalyses run(Function &function,
                        FunctionAnalysisManager &analysisManager);
};

struct RestoreSystolicMemoryStreamsPass
    : public PassInfoMixin<RestoreSystolicMemoryStreamsPass> {
  PreservedAnalyses run(Function &function,
                        FunctionAnalysisManager &analysisManager);
};

} // namespace morpher_systolic
