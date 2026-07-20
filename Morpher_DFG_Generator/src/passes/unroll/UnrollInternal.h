#pragma once

// Shared internals for the Morpher loop-unroll pass family. The original
// morpherLoopUnrollPass.cpp wrapped every helper in a single anonymous
// namespace; splitting it across translation units requires the items used by
// more than one .cpp to gain external linkage under a named namespace.

#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include <string>

namespace morpher_unroll {

using namespace llvm;

// Defined once in LoopUnroll.cpp; referenced by the marker/interchange passes.
extern cl::opt<std::string> MorpherUnrollFn;

inline constexpr StringLiteral MorpherStrippedMarkerAttr =
    "morpher.map-marker-stripped-depth";

inline Function *resolveCalledFunction(CallBase *CB) {
  if (!CB)
    return nullptr;

  Function *CalledFn = CB->getCalledFunction();
  if (CalledFn)
    return CalledFn;

  return dyn_cast<Function>(CB->getCalledOperand()->stripPointerCasts());
}

inline bool isPleaseMapMe(CallBase *CB) {
  Function *CalledFn = resolveCalledFunction(CB);
  return CalledFn && CalledFn->getName() == "please_map_me";
}

inline bool isPleaseMapMePeel(CallBase *CB) {
  Function *CalledFn = resolveCalledFunction(CB);
  return CalledFn && CalledFn->getName() == "please_map_me_peel";
}

inline void setIntMetadata(Instruction &I, StringRef Name, unsigned Value) {
  LLVMContext &Ctx = I.getContext();
  Metadata *MD = ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), Value));
  I.setMetadata(Name, MDNode::get(Ctx, MD));
}

inline void setSIMDMetadata(Instruction &I, unsigned Group, unsigned Lanes) {
  setIntMetadata(I, "morpher.simd.group", Group);
  setIntMetadata(I, "morpher.simd.lanes", Lanes);
}

inline CallBase *findAnnotatedLoopMarker(Function &F, LoopInfo &LI, Loop *&TargetLoop) {
  CallBase *Marker = nullptr;
  TargetLoop = nullptr;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallBase>(&I);
      if (!CI || !isPleaseMapMe(CI))
        continue;

      Loop *ContainingLoop = LI.getLoopFor(&BB);
      if (!ContainingLoop)
        report_fatal_error("please_map_me() is not inside a loop");

      if (Marker && ContainingLoop != TargetLoop)
        report_fatal_error("multiple please_map_me() calls annotate different loops");

      Marker = CI;
      TargetLoop = ContainingLoop;
    }
  }

  if (!Marker)
    report_fatal_error("could not find please_map_me() in target function");

  return Marker;
}

// Defined in SimdReductionExpansion.cpp; called from LoopUnroll.cpp.
bool expandReductionsForSIMD(Loop *L, unsigned Lanes, StringRef Policy);

// The marker/interchange passes are constructed by
// createMorpherLoopInterchangePipeline() (LoopInterchange.cpp), so their
// definitions must be visible across translation units. run() is defined
// out-of-line in MarkerPasses.cpp / LoopInterchange.cpp.
struct MorpherStripMapMarkerPass
    : public PassInfoMixin<MorpherStripMapMarkerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

struct MorpherInsertMapMarkerPass
    : public PassInfoMixin<MorpherInsertMapMarkerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

struct MorpherLoopInterchangePass
    : public PassInfoMixin<MorpherLoopInterchangePass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace morpher_unroll
