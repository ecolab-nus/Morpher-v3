// ConfigureSystolicUnrollJamPass: validate the transform recipe against the IR
// and attach unroll-and-jam loop metadata. Also hosts the full systolic
// unroll-and-jam pipeline factory. Split out of SystolicCandidatePass.cpp.

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopFuse.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Scalar/LoopUnrollAndJamPass.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

#include "SystolicInternal.h"

using namespace llvm;
using namespace morpher_systolic;

namespace morpher_systolic {

// Defined once here; declared extern in SystolicInternal.h.
cl::opt<unsigned> MorpherSystolicFactor0(
    "morpher-systolic-factor0", cl::init(0),
    cl::desc("Exact unroll-and-jam factor for the first parallel dimension"));

cl::opt<unsigned> MorpherSystolicFactor1(
    "morpher-systolic-factor1", cl::init(0),
    cl::desc("Exact unroll-and-jam factor for the second parallel dimension"));

} // namespace morpher_systolic

namespace {

void setLoopProperty(Loop *loop, StringRef name, ArrayRef<Metadata *> values,
                     bool removeUnrollDisable = false) {
  LLVMContext &context = loop->getHeader()->getContext();
  SmallVector<Metadata *, 8> operands;
  TempMDTuple selfReference = MDNode::getTemporary(context, std::nullopt);
  operands.push_back(selfReference.get());

  SmallVector<Metadata *, 4> propertyOperands;
  propertyOperands.push_back(MDString::get(context, name));
  propertyOperands.append(values.begin(), values.end());
  operands.push_back(MDNode::get(context, propertyOperands));

  if (MDNode *existing = loop->getLoopID()) {
    for (unsigned operandIndex = 1;
         operandIndex < existing->getNumOperands(); ++operandIndex) {
      auto *property =
          dyn_cast_or_null<MDNode>(existing->getOperand(operandIndex));
      StringRef propertyName;
      if (property && property->getNumOperands() != 0)
        if (auto *text = dyn_cast_or_null<MDString>(property->getOperand(0)))
          propertyName = text->getString();
      if (propertyName == name ||
          (removeUnrollDisable &&
           (propertyName == "llvm.loop.unroll.disable" ||
            propertyName == "llvm.loop.unroll_and_jam.disable")))
        continue;
      operands.push_back(existing->getOperand(operandIndex));
    }
  }

  MDNode *newLoopID = MDNode::getDistinct(context, operands);
  newLoopID->replaceOperandWith(0, newLoopID);
  loop->setLoopID(newLoopID);
}

} // namespace

namespace morpher_systolic {

PreservedAnalyses
ConfigureSystolicUnrollJamPass::run(Function &function,
                                   FunctionAnalysisManager &analysisManager) {
    if (function.isDeclaration())
      return PreservedAnalyses::all();
    if (MorpherSystolicTransformFunction.empty())
      report_fatal_error("morpher-systolic-fn must name the function to transform");
    if (function.getName() != MorpherSystolicTransformFunction)
      return PreservedAnalyses::all();
    if (MorpherSystolicDataflow != "output_stationary" &&
        MorpherSystolicDataflow != "weight_stationary")
      report_fatal_error(
          "selected systolic dataflow transform is not implemented");
    if (MorpherSystolicFactor0 <= 1 || MorpherSystolicFactor1 == 0)
      report_fatal_error("systolic unroll-and-jam factor0 must exceed one and factor1 must be positive");

    LoopInfo &loopInfo = analysisManager.getResult<LoopAnalysis>(function);
    ScalarEvolution &scalarEvolution =
        analysisManager.getResult<ScalarEvolutionAnalysis>(function);
    DominatorTree &dominatorTree =
        analysisManager.getResult<DominatorTreeAnalysis>(function);
    DependenceInfo &dependenceInfo =
        analysisManager.getResult<DependenceAnalysis>(function);

    Loop *outer = nullptr;
    ParsedTransformRecipe recipe;
    for (Loop *candidate : loopInfo.getLoopsInPreorder()) {
      auto parsed = parseTransformRecipe(candidate);
      if (!parsed || parsed->candidateIndex != MorpherSystolicCandidateIndex)
        continue;
      if (outer)
        report_fatal_error("systolic candidate index is ambiguous in target function");
      outer = candidate;
      recipe = *parsed;
    }
    if (!outer)
      report_fatal_error("requested function/candidate has no supported systolic transform recipe");

    SmallVector<Loop *, 4> levels = uniqueLoopChain(outer);
    const unsigned dimensionCount = recipe.dimensionLevels.size();
    if ((dimensionCount != 1 && dimensionCount != 2) ||
        levels.size() != dimensionCount + 1 ||
        recipe.reductionLevel != dimensionCount)
      report_fatal_error("systolic transform recipe no longer matches a canonical three-level nest");
    for (unsigned dimension = 0; dimension < dimensionCount; ++dimension)
      if (recipe.dimensionLevels[dimension] != dimension)
        report_fatal_error("systolic transform recipe dimensions are no longer canonical");
    if ((dimensionCount == 1 && MorpherSystolicFactor1 != 1) ||
        (dimensionCount == 2 && MorpherSystolicFactor1 <= 1))
      report_fatal_error("systolic factor1 must be one for dot recipes and exceed one for 2-D recipes");
    Attribute markerDepth = function.getFnAttribute(SystolicStrippedMarkerAttr);
    if (!markerDepth.isStringAttribute())
      report_fatal_error(
          "systolic transform requires please_map_me() in the rolled reduction loop");
    unsigned expectedDepth = 0;
    if (markerDepth.getValueAsString().getAsInteger(10, expectedDepth) ||
        levels[dimensionCount]->getLoopDepth() != expectedDepth)
      report_fatal_error("please_map_me() did not annotate the rolled reduction loop");

    const uint64_t expectedTrips[] = {
        recipe.dimensionTripCounts[0],
        dimensionCount == 2 ? recipe.dimensionTripCounts[1]
                            : recipe.reductionTripCount,
        recipe.reductionTripCount};
    for (unsigned level = 0; level < levels.size(); ++level) {
      auto currentTrip = constantTripCount(levels[level], scalarEvolution);
      if (!currentTrip || *currentTrip != expectedTrips[level])
        report_fatal_error("systolic transform recipe trip count no longer matches the IR");
    }
    if (MorpherSystolicDataflow == "weight_stationary") {
      if (dimensionCount != 2 ||
          MorpherSystolicFactor0 != recipe.reductionTripCount ||
          MorpherSystolicFactor1 != recipe.dimensionTripCounts[1])
        report_fatal_error(
            "weight-stationary v1 requires the reduction and second parallel dimension to exactly fill the spatial tile");
      for (BasicBlock *block : levels[0]->getBlocks())
        for (Instruction &instruction : *block)
          if ((instruction.getType()->isFloatingPointTy() &&
               (isAddShape(instruction.getOpcode()) ||
                isMulShape(instruction.getOpcode()))) ||
              (isa<IntrinsicInst>(instruction) &&
               (cast<IntrinsicInst>(instruction).getIntrinsicID() ==
                    Intrinsic::fmuladd ||
                cast<IntrinsicInst>(instruction).getIntrinsicID() ==
                    Intrinsic::fma)))
            report_fatal_error(
                "weight-stationary v1 is restricted to integer contractions; floating-point chain semantics are not yet defined");
      if (!isSafeToUnrollAndJam(levels[1], scalarEvolution, dominatorTree,
                                dependenceInfo, loopInfo))
        report_fatal_error(
            "LLVM rejected weight-stationary column/reduction unroll-and-jam legality");
      LLVMContext &context = function.getContext();
      Metadata *columnFactor = ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(context),
                           MorpherSystolicFactor1));
      setLoopProperty(levels[1], "llvm.loop.unroll_and_jam.count", columnFactor,
                      /*removeUnrollDisable=*/true);
      setLoopProperty(levels[2], "llvm.loop.unroll.full", {},
                      /*removeUnrollDisable=*/true);
      setLoopProperty(levels[0], SystolicReductionMetadata, {});
      errs() << "Morpher weight-stationary spatialization configured: function="
             << function.getName() << " candidate="
             << MorpherSystolicCandidateIndex << " reductionxcolumns="
             << MorpherSystolicFactor0 << "x" << MorpherSystolicFactor1
             << " stream=dimension0\n";
      return PreservedAnalyses::none();
    }

    if (recipe.dimensionTripCounts[0] % MorpherSystolicFactor0 != 0 ||
        (dimensionCount == 2 &&
         recipe.dimensionTripCounts[1] % MorpherSystolicFactor1 != 0))
      report_fatal_error("systolic unroll-and-jam factors must exactly divide their trip counts");
    if (!isSafeToUnrollAndJam(levels[0], scalarEvolution, dominatorTree,
                              dependenceInfo, loopInfo) ||
        (dimensionCount == 2 &&
         !isSafeToUnrollAndJam(levels[1], scalarEvolution, dominatorTree,
                               dependenceInfo, loopInfo)))
      report_fatal_error("LLVM rejected unroll-and-jam legality for the selected nest");

    LLVMContext &context = function.getContext();
    Metadata *factor0 = ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(context), MorpherSystolicFactor0));
    Metadata *factor1 = ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(context), MorpherSystolicFactor1));
    setLoopProperty(levels[0], "llvm.loop.unroll_and_jam.count", factor0,
                    /*removeUnrollDisable=*/true);
    if (dimensionCount == 2)
      setLoopProperty(levels[1], "llvm.loop.unroll_and_jam.count", factor1,
                      /*removeUnrollDisable=*/true);
    setLoopProperty(levels[dimensionCount], SystolicReductionMetadata, {});

    errs() << "Morpher systolic unroll-and-jam configured: function="
           << function.getName() << " candidate="
           << MorpherSystolicCandidateIndex << " factors="
           << MorpherSystolicFactor0 << "x" << MorpherSystolicFactor1 << "\n";
    return PreservedAnalyses::none();
}

} // namespace morpher_systolic

FunctionPassManager createMorpherSystolicUnrollJamPipeline() {
  FunctionPassManager FPM;
  FPM.addPass(LoopSimplifyPass());
  FPM.addPass(LCSSAPass());
  FPM.addPass(SystolicCandidatePass(/*emitJSONReport=*/false));
  FPM.addPass(StripSystolicMapMarkerPass());
  FPM.addPass(CanonicalizeSystolicOutputPass());
  FPM.addPass(ConfigureSystolicUnrollJamPass());
  FPM.addPass(createFunctionToLoopPassAdaptor(LoopUnrollAndJamPass()));
  FPM.addPass(createFunctionToLoopPassAdaptor(
      LoopFullUnrollPass(/*OptLevel=*/3, /*OnlyWhenForced=*/true)));
  FPM.addPass(LoopFusePass());
  FPM.addPass(createFunctionToLoopPassAdaptor(IndVarSimplifyPass()));
  FPM.addPass(InstCombinePass());
  FPM.addPass(EarlyCSEPass());
  FPM.addPass(GVNPass());
  FPM.addPass(SimplifyCFGPass());
  FPM.addPass(RestoreSystolicMemoryStreamsPass());
  FPM.addPass(RestoreSystolicMapMarkerPass());
  return FPM;
}
