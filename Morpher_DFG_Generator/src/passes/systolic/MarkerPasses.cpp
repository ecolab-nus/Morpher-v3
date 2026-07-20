// StripSystolicMapMarkerPass / RestoreSystolicMapMarkerPass: temporarily remove
// and later reinsert the please_map_me() marker across the systolic
// unroll-and-jam transform. Split out of the original SystolicCandidatePass.cpp.

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

#include "SystolicInternal.h"

using namespace llvm;
using namespace morpher_systolic;

namespace morpher_systolic {

// Defined once here; declared extern in SystolicInternal.h.
cl::opt<std::string> MorpherSystolicTransformFunction(
    "morpher-systolic-fn", cl::init(""),
    cl::desc("Function containing the systolic candidate to transform"));

} // namespace morpher_systolic

namespace {

Function *resolveCalledFunction(CallBase *call) {
  if (!call)
    return nullptr;
  if (Function *called = call->getCalledFunction())
    return called;
  return dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
}

bool isMapMarker(CallBase *call) {
  Function *called = resolveCalledFunction(call);
  return called && called->getName() == "please_map_me";
}

} // namespace

namespace morpher_systolic {

PreservedAnalyses
StripSystolicMapMarkerPass::run(Function &function,
                               FunctionAnalysisManager &analysisManager) {
    if (function.isDeclaration() ||
        function.getName() != MorpherSystolicTransformFunction)
      return PreservedAnalyses::all();

    LoopInfo &loopInfo = analysisManager.getResult<LoopAnalysis>(function);
    Loop *markerLoop = nullptr;
    SmallVector<CallBase *, 4> markers;
    for (BasicBlock &block : function) {
      for (Instruction &instruction : block) {
        auto *call = dyn_cast<CallBase>(&instruction);
        if (!call || !isMapMarker(call))
          continue;
        Loop *containingLoop = loopInfo.getLoopFor(&block);
        if (!containingLoop)
          report_fatal_error("please_map_me() must be inside the candidate nest");
        if (markerLoop && markerLoop != containingLoop)
          report_fatal_error("please_map_me() calls annotate different loops");
        if (!call->use_empty())
          report_fatal_error("please_map_me() return value cannot be used");
        markerLoop = containingLoop;
        markers.push_back(call);
      }
    }
    if (markers.empty())
      return PreservedAnalyses::all();

    // Once the detector has attached recipes, bind the source marker to the
    // explicitly selected candidate.  This replaces the old whole-function
    // single-nest restriction without allowing an unmarked sibling nest to be
    // transformed and receive the restored marker.
    Loop *selectedOuter = nullptr;
    for (Loop *candidate : loopInfo.getLoopsInPreorder()) {
      MDNode *property = findLoopProperty(candidate, SystolicRecipeMetadata);
      auto selected = metadataUnsigned(property, 2);
      if (!selected || *selected != MorpherSystolicCandidateIndex)
        continue;
      if (selectedOuter)
        report_fatal_error("systolic candidate index is ambiguous during marker binding");
      selectedOuter = candidate;
    }
    if (selectedOuter) {
      Loop *selectedReduction = selectedOuter;
      while (!selectedReduction->getSubLoops().empty()) {
        if (selectedReduction->getSubLoops().size() != 1)
          report_fatal_error(
              "selected systolic candidate is not a unique loop chain");
        selectedReduction = selectedReduction->getSubLoops().front();
      }
      if (markerLoop != selectedReduction)
        report_fatal_error(
            "selected systolic candidate does not own please_map_me() in its rolled reduction");
    }

    function.addFnAttr(SystolicStrippedMarkerAttr,
                       Twine(markerLoop->getLoopDepth()).str());
    for (CallBase *marker : markers)
      marker->eraseFromParent();
    errs() << "Morpher systolic unroll-and-jam temporarily removed "
           << markers.size() << " map marker(s) from " << function.getName()
           << "\n";
    return PreservedAnalyses::none();
}

PreservedAnalyses
RestoreSystolicMapMarkerPass::run(Function &function,
                                  FunctionAnalysisManager &analysisManager) {
    if (function.isDeclaration() ||
        function.getName() != MorpherSystolicTransformFunction)
      return PreservedAnalyses::all();

    LoopInfo &loopInfo = analysisManager.getResult<LoopAnalysis>(function);
    SmallVector<Loop *, 4> reductions;
    for (Loop *loop : loopInfo.getLoopsInPreorder())
      if (findLoopProperty(loop, SystolicReductionMetadata))
        reductions.push_back(loop);
    if (reductions.size() != 1)
      report_fatal_error(
          "systolic unroll-and-jam did not produce one rolled reduction loop");

    if (!function.hasFnAttribute(SystolicStrippedMarkerAttr))
      return PreservedAnalyses::all();
    for (BasicBlock &block : function)
      for (Instruction &instruction : block)
        if (auto *call = dyn_cast<CallBase>(&instruction))
          if (isMapMarker(call))
            report_fatal_error("please_map_me() unexpectedly survived marker stripping");

    Function *markerFunction = function.getParent()->getFunction("please_map_me");
    if (!markerFunction)
      report_fatal_error("please_map_me() declaration disappeared during systolic transform");
    IRBuilder<> builder(reductions.front()->getHeader()->getTerminator());
    builder.CreateCall(markerFunction->getFunctionType(), markerFunction);
    function.removeFnAttr(SystolicStrippedMarkerAttr);
    errs() << "Morpher systolic unroll-and-jam restored map marker in rolled "
              "reduction loop of "
           << function.getName() << "\n";
    return PreservedAnalyses::none();
}

} // namespace morpher_systolic

FunctionPassManager createMorpherSystolicSchedulePrepPipeline() {
  FunctionPassManager FPM;
  FPM.addPass(LoopSimplifyPass());
  FPM.addPass(LCSSAPass());
  FPM.addPass(StripSystolicMapMarkerPass());
  return FPM;
}
