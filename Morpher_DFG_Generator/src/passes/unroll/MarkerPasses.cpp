#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "UnrollInternal.h"

using namespace llvm;
using namespace morpher_unroll;

namespace {

void collectInnermostLoops(Loop *L, SmallVectorImpl<Loop *> &Innermost) {
  if (L->getSubLoops().empty()) {
    Innermost.push_back(L);
    return;
  }
  for (Loop *SubLoop : L->getSubLoops())
    collectInnermostLoops(SubLoop, Innermost);
}

SmallVector<Loop *, 4> findDeepestLoops(LoopInfo &LI) {
  SmallVector<Loop *, 8> Innermost;
  for (Loop *TopLevel : LI)
    collectInnermostLoops(TopLevel, Innermost);

  unsigned MaxDepth = 0;
  for (Loop *L : Innermost)
    MaxDepth = std::max(MaxDepth, L->getLoopDepth());

  SmallVector<Loop *, 4> Deepest;
  for (Loop *L : Innermost)
    if (L->getLoopDepth() == MaxDepth)
      Deepest.push_back(L);
  return Deepest;
}

} // namespace

namespace morpher_unroll {

PreservedAnalyses
MorpherStripMapMarkerPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration() ||
      (!MorpherUnrollFn.empty() && F.getName() != MorpherUnrollFn))
    return PreservedAnalyses::all();

  LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
  Loop *TargetLoop = nullptr;
  CallBase *Marker = findAnnotatedLoopMarker(F, LI, TargetLoop);
  SmallVector<Loop *, 4> Deepest = findDeepestLoops(LI);
  if (Deepest.size() != 1 || Deepest.front() != TargetLoop)
    report_fatal_error(
        "loop interchange requires please_map_me() to identify the unique deepest loop");

  for (Loop *L = TargetLoop; L->getParentLoop(); L = L->getParentLoop())
    if (L->getParentLoop()->getSubLoops().size() != 1)
      report_fatal_error(
          "loop interchange requires a single-child loop nest around please_map_me()");

  if (!Marker->use_empty())
    report_fatal_error("please_map_me() return value cannot be used");

  const unsigned Depth = TargetLoop->getLoopDepth();
  F.addFnAttr(MorpherStrippedMarkerAttr, Twine(Depth).str());
  Marker->eraseFromParent();
  errs() << "Morpher loop interchange: temporarily removed map marker from "
         << F.getName() << " at depth " << Depth << "\n";
  return PreservedAnalyses::none();
}

PreservedAnalyses
MorpherInsertMapMarkerPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration() ||
      (!MorpherUnrollFn.empty() && F.getName() != MorpherUnrollFn))
    return PreservedAnalyses::all();

  Attribute DepthAttr = F.getFnAttribute(MorpherStrippedMarkerAttr);
  if (!DepthAttr.isStringAttribute())
    return PreservedAnalyses::all();

  unsigned ExpectedDepth = 0;
  if (DepthAttr.getValueAsString().getAsInteger(10, ExpectedDepth))
    report_fatal_error("invalid stripped map-marker depth attribute");

  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (isPleaseMapMe(CB))
          report_fatal_error(
              "please_map_me() unexpectedly present before marker reinsertion");

  LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
  SmallVector<Loop *, 4> Deepest = findDeepestLoops(LI);
  if (Deepest.size() != 1 ||
      Deepest.front()->getLoopDepth() != ExpectedDepth)
    report_fatal_error(
        "loop interchange did not preserve one unambiguous deepest mapping loop");

  Function *MarkerFn = F.getParent()->getFunction("please_map_me");
  if (!MarkerFn)
    report_fatal_error("please_map_me() declaration disappeared during interchange");

  Instruction *InsertBefore = Deepest.front()->getHeader()->getTerminator();
  IRBuilder<> Builder(InsertBefore);
  Builder.CreateCall(MarkerFn->getFunctionType(), MarkerFn);
  F.removeFnAttr(MorpherStrippedMarkerAttr);
  errs() << "Morpher loop interchange: restored map marker in unique depth-"
         << ExpectedDepth << " loop of " << F.getName() << "\n";
  return PreservedAnalyses::none();
}

} // namespace morpher_unroll
