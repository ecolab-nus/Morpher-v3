// CanonicalizeSystolicOutputPass: convert the exact C[...] += product memory
// recurrence into a zero-seeded scalar reduction plus one epilogue update, prior
// to unroll-and-jam. Split out of the original SystolicCandidatePass.cpp.

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "SystolicInternal.h"

using namespace llvm;
using namespace morpher_systolic;

namespace {

// Detector/transform opt used only by this TU; keep file-local.
static cl::opt<bool> MorpherSystolicFPNondeterminism(
    "morpher-systolic-fp-nondeterminism", cl::init(true),
    cl::desc("Permit floating-point reassociation during systolic output canonicalization"));

} // namespace

namespace morpher_systolic {

// Convert the exact `C[...] += product` memory recurrence used by
// GEMM-like kernels into a zero-seeded scalar reduction plus one epilogue
// update of C.  Doing this before unroll-and-jam lets LLVM clone the scalar
// accumulator once per output lane, and lets the existing verified boundary
// canonicalizer fuse every cloned MUL+ADD into a stateful native MAC.
//
// Floating-point recurrences require an explicit opt-in because moving the
// dynamic C seed after the reduction changes IEEE/FMA rounding. Floating
// kernels can alternatively use the already-supported explicit
// `accumulator = 0` source form.
PreservedAnalyses
CanonicalizeSystolicOutputPass::run(Function &function,
                                    FunctionAnalysisManager &analysisManager) {
    if (function.isDeclaration() ||
        function.getName() != MorpherSystolicTransformFunction)
      return PreservedAnalyses::all();

    LoopInfo &loopInfo = analysisManager.getResult<LoopAnalysis>(function);
    Loop *outer = nullptr;
    ParsedTransformRecipe recipe;
    for (Loop *candidate : loopInfo.getLoopsInPreorder()) {
      auto parsed = parseTransformRecipe(candidate);
      if (!parsed || parsed->candidateIndex != MorpherSystolicCandidateIndex)
        continue;
      if (outer)
        report_fatal_error("systolic candidate index is ambiguous during output canonicalization");
      outer = candidate;
      recipe = *parsed;
    }
    if (!outer)
      report_fatal_error("systolic output canonicalization lost the transform recipe");

    SmallVector<Loop *, 4> levels = uniqueLoopChain(outer);
    if (levels.size() != recipe.dimensionLevels.size() + 1 ||
        (levels.size() != 2 && levels.size() != 3))
      report_fatal_error("systolic output canonicalization requires a supported reduction loop chain");
    Loop *reduction = levels.back();
    BasicBlock *header = reduction->getHeader();
    BasicBlock *preheader = reduction->getLoopPreheader();
    BasicBlock *latch = reduction->getLoopLatch();
    BasicBlock *exit = reduction->getUniqueExitBlock();
    BasicBlock *exiting = reduction->getExitingBlock();
    if (!header || !preheader || !latch || !exit || exiting != latch ||
        reduction->getNumBlocks() != 1)
      report_fatal_error(
          "systolic output canonicalization requires one canonical reduction block");

    SmallVector<StoreInst *, 4> stores;
    for (Instruction &instruction : *header)
      if (auto *store = dyn_cast<StoreInst>(&instruction))
        stores.push_back(store);

    unsigned promoted = 0;
    bool promotedFloating = false;
    for (StoreInst *store : stores) {
      if (!store->isSimple())
        continue;
      auto *update = dyn_cast<Instruction>(store->getValueOperand());
      if (!update)
        continue;
      auto *binaryUpdate = dyn_cast<BinaryOperator>(update);
      auto *intrinsicUpdate = dyn_cast<IntrinsicInst>(update);
      const bool isInteger = binaryUpdate &&
                             binaryUpdate->getOpcode() == Instruction::Add &&
                             update->getType()->isIntegerTy();
      const bool isFloatingAdd = binaryUpdate &&
                                 binaryUpdate->getOpcode() == Instruction::FAdd &&
                                 update->getType()->isFloatingPointTy();
      const bool isFloatingFMA = intrinsicUpdate &&
                                 intrinsicUpdate->getIntrinsicID() == Intrinsic::fmuladd &&
                                 update->getType()->isFloatingPointTy();
      const bool isFloating = isFloatingAdd || isFloatingFMA;
      if (!isInteger && !(isFloating && MorpherSystolicFPNondeterminism))
        continue;

      LoadInst *seedLoad = nullptr;
      unsigned seedOperand = 0;
      const unsigned seedBegin = isFloatingFMA ? 2 : 0;
      const unsigned seedEnd = isFloatingFMA ? 3 : 2;
      for (unsigned operand = seedBegin; operand < seedEnd; ++operand) {
        auto *load = dyn_cast<LoadInst>(update->getOperand(operand));
        if (!load || !load->isSimple() ||
            load->getPointerOperand() != store->getPointerOperand())
          continue;
        seedLoad = load;
        seedOperand = operand;
        break;
      }
      if (!seedLoad || !seedLoad->hasOneUse() || !update->hasOneUse() ||
          !reduction->isLoopInvariant(store->getPointerOperand()))
        continue;

      PHINode *accumulator = PHINode::Create(
          update->getType(), 2, "morpher.systolic.acc", &*header->begin());
      accumulator->addIncoming(Constant::getNullValue(update->getType()), preheader);
      accumulator->addIncoming(update, latch);
      update->setOperand(seedOperand, accumulator);
      // The zero-based integer partial sum can overflow in cases where the
      // original C-biased intermediate did not. Modular ADD still agrees for
      // every source execution in which the original signed operations were
      // defined, so stale source no-wrap flags must not survive.
      if (isInteger) {
        binaryUpdate->setHasNoSignedWrap(false);
        binaryUpdate->setHasNoUnsignedWrap(false);
      }

      IRBuilder<> builder(&*exit->getFirstInsertionPt());
      LoadInst *initialC = builder.CreateLoad(
          seedLoad->getType(), store->getPointerOperand(),
          "morpher.systolic.initial_c");
      initialC->setAlignment(seedLoad->getAlign());
      initialC->copyMetadata(*seedLoad);
      Value *finalC = isInteger
          ? builder.CreateAdd(initialC, update, "morpher.systolic.final_c")
          : builder.CreateFAdd(initialC, update, "morpher.systolic.final_c");
      StoreInst *finalStore = builder.CreateStore(finalC, store->getPointerOperand());
      finalStore->setAlignment(store->getAlign());
      finalStore->copyMetadata(*store);

      store->eraseFromParent();
      seedLoad->eraseFromParent();
      promotedFloating |= isFloating;
      ++promoted;
    }

    if (promoted == 0) {
      errs() << "Morpher systolic output canonicalization: no supported memory "
                "recurrence in " << function.getName() << "\n";
      return PreservedAnalyses::all();
    }
    if (promoted != 1)
      report_fatal_error(
          "systolic v1 expected exactly one pre-jam output recurrence");
    errs() << "Morpher systolic output canonicalization promoted " << promoted
           << " output recurrence in " << function.getName();
    if (promotedFloating)
      errs() << " (fp_nondeterminism enabled)";
    errs() << "\n";
    return PreservedAnalyses::none();
}

} // namespace morpher_systolic
