#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar/LoopInterchange.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"

#include "UnrollInternal.h"

using namespace llvm;
using namespace morpher_unroll;

namespace morpher_unroll {

PreservedAnalyses
MorpherLoopInterchangePass::run(Function &F, FunctionAnalysisManager &AM) {
  if (F.isDeclaration() ||
      (!MorpherUnrollFn.empty() && F.getName() != MorpherUnrollFn))
    return PreservedAnalyses::all();
  return createFunctionToLoopPassAdaptor(LoopInterchangePass()).run(F, AM);
}

} // namespace morpher_unroll

FunctionPassManager createMorpherLoopInterchangePipeline() {
  FunctionPassManager FPM;
  FPM.addPass(MorpherStripMapMarkerPass());
  FPM.addPass(MorpherLoopInterchangePass());
  FPM.addPass(MorpherInsertMapMarkerPass());
  return FPM;
}
