#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

using namespace llvm;

namespace {

static cl::opt<std::string> MorpherUnrollFn(
    "morpher-unroll-fn", cl::init(""),
    cl::desc("Function containing the Morpher loop to unroll"));

static cl::opt<unsigned> MorpherUnrollFactor(
    "morpher-unroll-factor", cl::init(1),
    cl::desc("Scalar unroll factor for the Morpher annotated loop"));

static cl::opt<std::string> MorpherRemainderPolicy(
    "morpher-remainder-policy", cl::init("preserve"),
    cl::desc("Remainder policy: preserve or assume-multiple"));

Function *getCalledFunctionThroughBitcast(CallInst *CI) {
  Function *CalledFn = CI->getCalledFunction();
  if (CalledFn)
    return CalledFn;

  if (auto *CE = dyn_cast<ConstantExpr>(CI->getCalledOperand()))
    if (CE->getOpcode() == Instruction::BitCast)
      return dyn_cast<Function>(CE->getOperand(0));

  return nullptr;
}

bool isPleaseMapMe(CallInst *CI) {
  Function *CalledFn = getCalledFunctionThroughBitcast(CI);
  return CalledFn && CalledFn->getName() == "please_map_me";
}

void setIntMetadata(Instruction &I, StringRef Name, unsigned Value) {
  LLVMContext &Ctx = I.getContext();
  Metadata *MD = ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), Value));
  I.setMetadata(Name, MDNode::get(Ctx, MD));
}

void annotateLoopInstructionsForSIMD(Loop *TargetLoop, unsigned Lanes) {
  unsigned Group = 0;
  for (BasicBlock *BB : TargetLoop->blocks()) {
    for (Instruction &I : *BB) {
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (isPleaseMapMe(CI))
          continue;
      setIntMetadata(I, "morpher.simd.group", Group++);
      setIntMetadata(I, "morpher.simd.lanes", Lanes);
    }
  }
}

CallInst *findAnnotatedLoopMarker(Function &F, LoopInfo &LI, Loop *&TargetLoop) {
  CallInst *Marker = nullptr;
  TargetLoop = nullptr;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
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

bool normalizePleaseMapMeMarkers(Function &F, LoopInfo &LI, Loop *TargetLoop) {
  SmallVector<CallInst *, 8> Markers;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (isPleaseMapMe(CI))
          Markers.push_back(CI);

  if (Markers.empty())
    report_fatal_error("unrolled loop no longer contains please_map_me()");

  CallInst *Keep = nullptr;
  for (CallInst *Marker : Markers) {
    BasicBlock *BB = Marker->getParent();
    if (TargetLoop && TargetLoop->contains(BB)) {
      Keep = Marker;
      break;
    }
  }

  if (!Keep)
    Keep = Markers.front();

  bool Changed = false;
  for (CallInst *Marker : Markers) {
    if (Marker == Keep)
      continue;
    Marker->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

struct MorpherLoopUnrollPass : public FunctionPass {
  static char ID;

  MorpherLoopUnrollPass() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
    if (MorpherUnrollFactor <= 1)
      return false;

    if (!MorpherUnrollFn.empty() && F.getName() != MorpherUnrollFn)
      return false;

    if (MorpherRemainderPolicy != "preserve" &&
        MorpherRemainderPolicy != "assume-multiple") {
      report_fatal_error("morpher-remainder-policy must be preserve or assume-multiple");
    }

    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    ScalarEvolution &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    AssumptionCache &AC =
        getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);

    Loop *TargetLoop = nullptr;
    findAnnotatedLoopMarker(F, LI, TargetLoop);

    unsigned TripCount = SE.getSmallConstantTripCount(TargetLoop);
    if (TripCount != 0 && MorpherUnrollFactor >= TripCount) {
      report_fatal_error(
          "Morpher unroll factor must be smaller than the known loop trip count");
    }

    unsigned MaxTripCount = SE.getSmallConstantMaxTripCount(TargetLoop);
    if (MaxTripCount != 0 && MorpherUnrollFactor >= MaxTripCount) {
      report_fatal_error(
          "Morpher unroll factor must be smaller than the known max loop trip count");
    }

    annotateLoopInstructionsForSIMD(TargetLoop, MorpherUnrollFactor);

    UnrollLoopOptions ULO;
    ULO.Count = MorpherUnrollFactor;
    ULO.TripCount = 0;
    ULO.Force = true;
    ULO.AllowRuntime = MorpherRemainderPolicy == "preserve";
    ULO.AllowExpensiveTripCount = true;
    ULO.PreserveCondBr = false;
    ULO.PreserveOnlyFirst = false;
    ULO.TripMultiple = 1;
    ULO.PeelCount = 0;
    ULO.UnrollRemainder = false;
    ULO.ForgetAllSCEV = true;

    LoopUnrollResult Result =
        UnrollLoop(TargetLoop, ULO, &LI, &SE, &DT, &AC, nullptr, true);

    if (Result == LoopUnrollResult::Unmodified)
      report_fatal_error("LLVM could not unroll the Morpher annotated loop");

    if (Result == LoopUnrollResult::FullyUnrolled)
      report_fatal_error("Morpher annotated loop was fully unrolled away");

    normalizePleaseMapMeMarkers(F, LI, TargetLoop);

    errs() << "Morpher loop unroll: function=" << F.getName()
           << " factor=" << MorpherUnrollFactor
           << " remainder_policy=" << MorpherRemainderPolicy << "\n";
    return true;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<AssumptionCacheTracker>();
  }
};

} // namespace

char MorpherLoopUnrollPass::ID = 0;
static RegisterPass<MorpherLoopUnrollPass>
    X("morpher-loop-unroll", "Morpher annotated scalar loop unroll", false,
      false);
