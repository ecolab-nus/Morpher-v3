#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
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

static cl::opt<unsigned> MorpherReductionLanes(
    "morpher-reduction-lanes", cl::init(1),
    cl::desc("SIMD width for scalar reduction expansion after unroll"));

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

bool isSupportedReductionOpcode(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    return true;
  default:
    return false;
  }
}

Value *identityForReduction(unsigned Opcode, Type *Ty) {
  switch (Opcode) {
  case Instruction::Add:
  case Instruction::Sub:
    if (Ty->isIntegerTy())
      return ConstantInt::get(Ty, 0);
    break;
  case Instruction::Mul:
    if (Ty->isIntegerTy())
      return ConstantInt::get(Ty, 1);
    break;
  case Instruction::And:
    if (Ty->isIntegerTy())
      return ConstantInt::getAllOnesValue(Ty);
    break;
  case Instruction::Or:
  case Instruction::Xor:
    if (Ty->isIntegerTy())
      return ConstantInt::get(Ty, 0);
    break;
  default:
    break;
  }
  return nullptr;
}

unsigned termCombineOpcode(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Sub:
    return Instruction::Add;
  default:
    return Opcode;
  }
}

unsigned finalCombineOpcode(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Sub:
    return Instruction::Add;
  default:
    return Opcode;
  }
}

Value *createReductionOp(IRBuilder<> &Builder, unsigned Opcode, Value *LHS,
                         Value *RHS, const Twine &Name = "") {
  switch (Opcode) {
  case Instruction::Add:
    return Builder.CreateAdd(LHS, RHS, Name);
  case Instruction::Sub:
    return Builder.CreateSub(LHS, RHS, Name);
  case Instruction::Mul:
    return Builder.CreateMul(LHS, RHS, Name);
  case Instruction::And:
    return Builder.CreateAnd(LHS, RHS, Name);
  case Instruction::Or:
    return Builder.CreateOr(LHS, RHS, Name);
  case Instruction::Xor:
    return Builder.CreateXor(LHS, RHS, Name);
  default:
    llvm_unreachable("unsupported reduction opcode");
  }
}

bool collectReductionAddChain(
    PHINode *Phi,
    Value *BackedgeValue,
    unsigned &ReductionOpcode,
    SmallVectorImpl<BinaryOperator *> &Chain,
    SmallVectorImpl<Value *> &Terms) {
  Value *Current = BackedgeValue;
  SmallVector<BinaryOperator *, 32> ReverseChain;
  SmallVector<Value *, 32> ReverseTerms;
  ReductionOpcode = 0;

  while (Current != Phi) {
    auto *BO = dyn_cast<BinaryOperator>(Current);
    if (!BO || !isSupportedReductionOpcode(BO->getOpcode()))
      return false;
    if (ReductionOpcode == 0)
      ReductionOpcode = BO->getOpcode();
    if (BO->getOpcode() != ReductionOpcode)
      return false;

    Value *LHS = BO->getOperand(0);
    Value *RHS = BO->getOperand(1);
    Value *Prev = nullptr;
    Value *Term = nullptr;

    if (ReductionOpcode == Instruction::Sub) {
      if (LHS == Phi || isa<BinaryOperator>(LHS)) {
        Prev = LHS;
        Term = RHS;
      } else {
        return false;
      }
    } else if (LHS == Phi || isa<BinaryOperator>(LHS)) {
      Prev = LHS;
      Term = RHS;
    } else if (RHS == Phi || isa<BinaryOperator>(RHS)) {
      Prev = RHS;
      Term = LHS;
    } else {
      return false;
    }

    if (BO != BackedgeValue && !BO->hasOneUse())
      return false;

    ReverseChain.push_back(BO);
    ReverseTerms.push_back(Term);
    Current = Prev;
  }

  for (auto It = ReverseChain.rbegin(); It != ReverseChain.rend(); ++It)
    Chain.push_back(*It);
  for (auto It = ReverseTerms.rbegin(); It != ReverseTerms.rend(); ++It)
    Terms.push_back(*It);
  return !Chain.empty();
}

bool expandIntegerAddReduction(PHINode *Phi, Loop *L, unsigned Lanes) {
  if (Lanes <= 1)
    return false;

  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Latch = L->getLoopLatch();
  if (!Preheader || !Latch)
    return false;

  int PreheaderIdx = Phi->getBasicBlockIndex(Preheader);
  int LatchIdx = Phi->getBasicBlockIndex(Latch);
  if (PreheaderIdx < 0 || LatchIdx < 0)
    return false;

  Value *Init = Phi->getIncomingValue(PreheaderIdx);
  Value *BackedgeValue = Phi->getIncomingValue(LatchIdx);
  if (!Phi->getType()->isIntegerTy())
    return false;
  if (!Phi->hasOneUse())
    return false;

  SmallVector<BinaryOperator *, 32> Chain;
  SmallVector<Value *, 32> Terms;
  unsigned ReductionOpcode = 0;
  if (!collectReductionAddChain(Phi, BackedgeValue, ReductionOpcode, Chain, Terms))
    return false;

  bool HasNonConstantTerm = false;
  for (Value *Term : Terms)
    if (!isa<ConstantInt>(Term)) {
      HasNonConstantTerm = true;
      break;
    }
  if (!HasNonConstantTerm)
    return false;

  if (Terms.size() < Lanes || Terms.size() % Lanes != 0)
    return false;

  unsigned PartialCount = Terms.size() / Lanes;
  Value *Identity = identityForReduction(ReductionOpcode, Phi->getType());
  if (!Identity)
    return false;

  IRBuilder<> PhiBuilder(&*Phi->getParent()->getFirstInsertionPt());
  SmallVector<PHINode *, 16> Partials;
  for (unsigned I = 0; I < PartialCount; ++I) {
    PHINode *Partial = PhiBuilder.CreatePHI(Phi->getType(), 2,
                                            Phi->getName() + ".morpher.red");
    Partial->addIncoming(I == 0 ? Init : Identity, Preheader);
    Partials.push_back(Partial);
  }

  IRBuilder<> BodyBuilder(Chain.back());
  SmallVector<Value *, 16> Updates;
  for (unsigned P = 0; P < PartialCount; ++P) {
    unsigned Start = P * Lanes;
    Value *Local = Terms[Start];
    for (unsigned I = 1; I < Lanes; ++I) {
      Local = createReductionOp(BodyBuilder, termCombineOpcode(ReductionOpcode), Local,
                                Terms[Start + I], "morpher.red.local");
    }
    Value *Update = createReductionOp(BodyBuilder, ReductionOpcode, Partials[P],
                                      Local, "morpher.red.update");
    Updates.push_back(Update);
    Partials[P]->addIncoming(Update, Latch);
  }

  Value *Combined = Updates[0];
  for (unsigned I = 1; I < Updates.size(); ++I)
    Combined = createReductionOp(BodyBuilder, finalCombineOpcode(ReductionOpcode), Combined,
                                 Updates[I], "morpher.red.combine");

  BackedgeValue->replaceAllUsesWith(Combined);
  errs() << "Morpher reduction expansion: phi=" << Phi->getName()
         << " lanes=" << Lanes
         << " terms=" << Terms.size()
         << " partials=" << PartialCount << "\n";
  return true;
}

bool expandReductionsForSIMD(Loop *L, unsigned Lanes) {
  if (Lanes <= 1)
    return false;

  SmallVector<PHINode *, 8> Phis;
  for (PHINode &Phi : L->getHeader()->phis())
    Phis.push_back(&Phi);

  bool Changed = false;
  for (PHINode *Phi : Phis)
    Changed |= expandIntegerAddReduction(Phi, L, Lanes);
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

    expandReductionsForSIMD(TargetLoop, MorpherReductionLanes);
    normalizePleaseMapMeMarkers(F, LI, TargetLoop);

    errs() << "Morpher loop unroll: function=" << F.getName()
           << " factor=" << MorpherUnrollFactor
           << " remainder_policy=" << MorpherRemainderPolicy
           << " reduction_lanes=" << MorpherReductionLanes << "\n";
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
