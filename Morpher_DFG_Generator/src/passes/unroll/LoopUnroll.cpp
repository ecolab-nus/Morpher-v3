#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Scalar/LoopInterchange.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include <map>

#include "UnrollInternal.h"

using namespace llvm;
using namespace morpher_unroll;

namespace morpher_unroll {

// Sole definition of the shared option; the marker/interchange TUs see it via
// the `extern` declaration in UnrollInternal.h.
cl::opt<std::string> MorpherUnrollFn(
    "morpher-unroll-fn", cl::init(""),
    cl::desc("Function containing the Morpher loop to unroll"));

} // namespace morpher_unroll

namespace {

static cl::opt<unsigned> MorpherUnrollFactor(
    "morpher-unroll-factor", cl::init(1),
    cl::desc("Scalar unroll factor for the Morpher annotated loop"));

static cl::opt<bool> MorpherFuseInnermostLoops(
    "morpher-fuse-innermost-loops", cl::init(false),
    cl::desc("Run LLVM loop-flattening before Morpher unroll to fuse legal inner loop nests"));

static cl::opt<unsigned> MorpherReductionLanes(
    "morpher-reduction-lanes", cl::init(1),
    cl::desc("Architecture-derived SIMD lane count for reduction lowering"));

static cl::opt<bool> MorpherPromoteMemoryReductions(
    "morpher-promote-memory-reductions", cl::init(false),
    cl::desc("Promote invariant-address load/update/store reductions to SSA "
             "before unrolling; independent of SIMD width"));

static cl::opt<std::string> MorpherReductionPolicy(
    "morpher-reduction-policy", cl::init("tree"),
    cl::desc("Reduction lowering policy: tree, packed-psum, scalar-psum, or vector-psum"));

// Loop-peel P/E: opt-in retagging of the runtime remainder loop's marker.
// Default off until Phase 2b (peel DFG emission in dfggenPass) is robust
// against the i64 OutLoopSTORE case in `DFG::nameNodes`.
static cl::opt<bool> MorpherEmitPeel(
    "morpher-emit-peel", cl::init(false),
    cl::desc("Tag the runtime remainder loop with please_map_me_peel for "
             "downstream peel DFG emission. When false, mirrors the legacy "
             "behavior of dropping the orphan marker."));

// Inserts (or returns the existing) `void please_map_me_peel(void)` declaration
// in the given module. The runtime body is never invoked: dfggenPass erases the
// marker call once it has located the peel loop's mapping unit.
Function *getOrInsertPleaseMapMePeel(Module &M) {
  LLVMContext &Ctx = M.getContext();
  FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), /*isVarArg=*/false);
  FunctionCallee Callee = M.getOrInsertFunction("please_map_me_peel", FT);
  return cast<Function>(Callee.getCallee());
}

// Rewrite any `please_map_me()` calls inside the LLVM-generated remainder loop
// (the un-unrolled "epilogue" loop that runs the leftover (TC % U) iterations
// of the original body) to `please_map_me_peel()`. This survives the existing
// `normalizePleaseMapMeMarkers` pass because that helper exact-matches on the
// "please_map_me" symbol name. Downstream dfggenPass picks up the peel marker
// and emits a separate DFG for the peel mapping unit.
unsigned retagRemainderMarkersAsPeel(Loop *RemainderLoop) {
  if (!RemainderLoop)
    return 0;
  Module &M = *RemainderLoop->getHeader()->getModule();
  Function *PeelFn = getOrInsertPleaseMapMePeel(M);
  unsigned retagged = 0;
  for (BasicBlock *BB : RemainderLoop->blocks()) {
    for (Instruction &I : *BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || !isPleaseMapMe(CB))
        continue;
      CB->setCalledFunction(PeelFn);
      ++retagged;
    }
  }
  return retagged;
}

int readIntMetadata(const Instruction &I, StringRef Name, int DefaultValue) {
  MDNode *MD = I.getMetadata(Name);
  if (!MD || MD->getNumOperands() == 0)
    return DefaultValue;
  auto *CAM = dyn_cast<ConstantAsMetadata>(MD->getOperand(0));
  auto *CI = CAM ? dyn_cast<ConstantInt>(CAM->getValue()) : nullptr;
  return CI ? static_cast<int>(CI->getSExtValue()) : DefaultValue;
}

bool loopIDHasSystolicCandidate(MDNode *LoopID) {
  if (!LoopID)
    return false;
  for (unsigned i = 1; i < LoopID->getNumOperands(); ++i) {
    auto *MD = dyn_cast<MDNode>(LoopID->getOperand(i));
    if (!MD || MD->getNumOperands() == 0)
      continue;
    auto *S = dyn_cast<MDString>(MD->getOperand(0));
    if (S && S->getString() == "systolic.candidate")
      return true;
  }
  return false;
}

bool loopCarriesSystolicCandidate(Loop *L) {
  for (Loop *Cur = L; Cur != nullptr; Cur = Cur->getParentLoop()) {
    if (loopIDHasSystolicCandidate(Cur->getLoopID()))
      return true;
  }
  return false;
}

void annotateLoopInstructionsForSIMD(Loop *TargetLoop, unsigned Lanes) {
  unsigned Group = 0;
  for (BasicBlock *BB : TargetLoop->blocks()) {
    for (Instruction &I : *BB) {
      if (auto *CI = dyn_cast<CallBase>(&I))
        if (isPleaseMapMe(CI))
          continue;
      setSIMDMetadata(I, Group++, Lanes);
    }
  }
}

void splitUnrolledSIMDGroups(Loop *TargetLoop, unsigned Lanes) {
  if (!TargetLoop || Lanes <= 1)
    return;

  std::map<unsigned, SmallVector<Instruction *, 16>> Groups;
  unsigned NextGroup = 0;
  for (BasicBlock *BB : TargetLoop->blocks()) {
    for (Instruction &I : *BB) {
      int Group = readIntMetadata(I, "morpher.simd.group", -1);
      if (Group < 0)
        continue;
      Groups[static_cast<unsigned>(Group)].push_back(&I);
      NextGroup = std::max(NextGroup, static_cast<unsigned>(Group) + 1);
    }
  }

  unsigned SplitGroups = 0;
  for (auto &Entry : Groups) {
    SmallVector<Instruction *, 16> &Members = Entry.second;
    if (Members.size() <= Lanes)
      continue;
    if (Members.size() % Lanes != 0) {
      errs() << "Morpher SIMD group split skipped: group=" << Entry.first
             << " members=" << Members.size()
             << " lanes=" << Lanes << "\n";
      continue;
    }
    for (unsigned Lane = 0; Lane < Lanes; ++Lane)
      setSIMDMetadata(*Members[Lane], Entry.first, Lanes);
    for (unsigned Start = Lanes; Start < Members.size(); Start += Lanes) {
      unsigned ChunkGroup = NextGroup++;
      for (unsigned Lane = 0; Lane < Lanes; ++Lane)
        setSIMDMetadata(*Members[Start + Lane], ChunkGroup, Lanes);
      ++SplitGroups;
    }
  }

  if (SplitGroups != 0)
    errs() << "Morpher SIMD group split: additional_vector_groups="
           << SplitGroups << " lanes=" << Lanes << "\n";
}

bool normalizePleaseMapMeMarkers(Function &F, LoopInfo &LI, Loop *TargetLoop) {
  SmallVector<CallBase *, 8> Markers;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CI = dyn_cast<CallBase>(&I))
        if (isPleaseMapMe(CI))
          Markers.push_back(CI);

  if (Markers.empty())
    report_fatal_error("unrolled loop no longer contains please_map_me()");

  CallBase *Keep = nullptr;
  for (CallBase *Marker : Markers) {
    BasicBlock *BB = Marker->getParent();
    if (TargetLoop && TargetLoop->contains(BB)) {
      Keep = Marker;
      break;
    }
  }

  if (!Keep)
    Keep = Markers.front();

  bool Changed = false;
  for (CallBase *Marker : Markers) {
    if (Marker == Keep)
      continue;
    Marker->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

bool sameArrayRowsKnownDifferent(Value *A, Value *B, Loop *L,
                                 ScalarEvolution &SE) {
  auto *GEPA = dyn_cast<GetElementPtrInst>(A);
  auto *GEPB = dyn_cast<GetElementPtrInst>(B);
  if (!GEPA || !GEPB)
    return false;
  if (GEPA->getPointerOperand() != GEPB->getPointerOperand())
    return false;
  if (GEPA->getSourceElementType() != GEPB->getSourceElementType())
    return false;
  if (!GEPA->getSourceElementType()->isArrayTy())
    return false;

  SmallVector<Value *, 4> IndicesA(GEPA->idx_begin(), GEPA->idx_end());
  SmallVector<Value *, 4> IndicesB(GEPB->idx_begin(), GEPB->idx_end());
  if (IndicesA.size() < 3 || IndicesB.size() < 3)
    return false;

  // For a normal 2D static array GEP, the first index is commonly the
  // leading zero into the global object.  The row coordinate is the
  // penultimate index, and the final index is the column coordinate.
  Value *RowIndexA = IndicesA[IndicesA.size() - 2];
  Value *RowIndexB = IndicesB[IndicesB.size() - 2];
  const SCEV *RowA = SE.getSCEV(RowIndexA);
  const SCEV *RowB = SE.getSCEV(RowIndexB);
  if (SE.isKnownPredicate(CmpInst::ICMP_NE, RowA, RowB))
    return true;

  auto rowAddRecStartsAboveInvariant =
      [&](const SCEV *MaybeRec, const SCEV *MaybeInvariant,
          Value *MaybeRecIndex, Value *MaybeInvariantIndex) {
        auto *AR = dyn_cast<SCEVAddRecExpr>(MaybeRec);
        if (!AR || AR->getLoop() != L)
          return false;
        if (!L->isLoopInvariant(MaybeInvariantIndex) ||
            L->isLoopInvariant(MaybeRecIndex))
          return false;
        const SCEV *Start = AR->getStart();
        const SCEV *Step = AR->getStepRecurrence(SE);
        return SE.isKnownPredicate(CmpInst::ICMP_SGT, Start, MaybeInvariant) &&
               SE.isKnownPositive(Step);
      };

  return rowAddRecStartsAboveInvariant(RowA, RowB, RowIndexA, RowIndexB) ||
         rowAddRecStartsAboveInvariant(RowB, RowA, RowIndexB, RowIndexA);
}

bool accessesKnownIndependent(Value *PtrA, MemoryLocation LocA, Value *PtrB,
                              MemoryLocation LocB, Loop *L,
                              ScalarEvolution &SE, AAResults &AA) {
  if (AA.alias(LocA, LocB) == AliasResult::NoAlias)
    return true;
  return sameArrayRowsKnownDifferent(PtrA, PtrB, L, SE);
}

Value *findStoredPreheaderValue(Value *Ptr, Type *ValueType,
                                BasicBlock *Preheader, AAResults &AA) {
  Value *StoredValue = nullptr;
  MemoryLocation PtrLoc = MemoryLocation::getBeforeOrAfter(Ptr);

  for (Instruction &I : *Preheader) {
    if (I.isTerminator())
      break;

    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (SI->isVolatile() || SI->isAtomic()) {
        StoredValue = nullptr;
        continue;
      }

      MemoryLocation StoreLoc = MemoryLocation::get(SI);
      AliasResult AR = AA.alias(PtrLoc, StoreLoc);
      if ((SI->getPointerOperand() == Ptr || AR == AliasResult::MustAlias) &&
          SI->getValueOperand()->getType() == ValueType) {
        StoredValue = SI->getValueOperand();
        continue;
      }
      if (AR != AliasResult::NoAlias)
        StoredValue = nullptr;
      continue;
    }

    if (I.mayWriteToMemory())
      StoredValue = nullptr;
  }

  return StoredValue;
}

bool scalarizeInvariantAddressReductionForLoop(Loop *L, ScalarEvolution &SE,
                                               DominatorTree &DT, AAResults &AA) {
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *ExitBlock = L->getExitBlock();
  if (!Header || !Latch || !Preheader || !ExitBlock)
    return false;
  if (L->getBlocks().size() != 1 || Header != Latch)
    return false;

  for (Instruction &I : *Header) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (isPleaseMapMe(CB))
        continue;
      return false;
    }
  }

  struct AccessGroup {
    Type *ValueType = nullptr;
    SmallVector<LoadInst *, 4> Loads;
    SmallVector<StoreInst *, 4> Stores;
  };

  std::map<Value *, AccessGroup> Groups;

  for (Instruction &I : *Header) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (LI->isVolatile() || LI->isAtomic())
        continue;
      Type *Ty = LI->getType();
      if (!Ty->isIntegerTy())
        continue;
      Value *Ptr = LI->getPointerOperand();
      if (!L->isLoopInvariant(Ptr))
        continue;
      AccessGroup &Group = Groups[Ptr];
      if (!Group.ValueType)
        Group.ValueType = Ty;
      if (Group.ValueType != Ty)
        continue;
      Group.Loads.push_back(LI);
      continue;
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (SI->isVolatile() || SI->isAtomic())
        continue;
      Type *Ty = SI->getValueOperand()->getType();
      if (!Ty->isIntegerTy())
        continue;
      Value *Ptr = SI->getPointerOperand();
      if (!L->isLoopInvariant(Ptr))
        continue;
      AccessGroup &Group = Groups[Ptr];
      if (!Group.ValueType)
        Group.ValueType = Ty;
      if (Group.ValueType != Ty)
        continue;
      Group.Stores.push_back(SI);
    }
  }

  bool Changed = false;
  for (auto &Entry : Groups) {
    Value *Ptr = Entry.first;
    AccessGroup &Group = Entry.second;
    if (Group.Loads.size() != 1 || Group.Stores.size() != 1 || !Group.ValueType)
      continue;

    LoadInst *LoopLoad = Group.Loads.front();
    StoreInst *LoopStore = Group.Stores.front();
    if (LoopLoad->getParent() != Header || LoopStore->getParent() != Header)
      continue;
    if (!DT.dominates(LoopLoad, LoopStore))
      continue;
    if (LoopStore->getValueOperand() == LoopLoad)
      continue;

    MemoryLocation LoadLoc = MemoryLocation::get(LoopLoad);
    MemoryLocation StoreLoc = MemoryLocation::get(LoopStore);
    bool ConflictingAccess = false;
    for (BasicBlock *BB : L->blocks()) {
      for (Instruction &I : *BB) {
        if (&I == LoopLoad || &I == LoopStore)
          continue;
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          if (LI->isVolatile() || LI->isAtomic()) {
            ConflictingAccess = true;
            break;
          }
          MemoryLocation OtherLoc = MemoryLocation::get(LI);
          Value *OtherPtr = LI->getPointerOperand();
          if (!accessesKnownIndependent(Ptr, LoadLoc, OtherPtr, OtherLoc, L,
                                        SE, AA) ||
              !accessesKnownIndependent(Ptr, StoreLoc, OtherPtr, OtherLoc, L,
                                        SE, AA)) {
            ConflictingAccess = true;
            break;
          }
          continue;
        }
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          if (SI->isVolatile() || SI->isAtomic()) {
            ConflictingAccess = true;
            break;
          }
          MemoryLocation OtherLoc = MemoryLocation::get(SI);
          Value *OtherPtr = SI->getPointerOperand();
          if (!accessesKnownIndependent(Ptr, LoadLoc, OtherPtr, OtherLoc, L,
                                        SE, AA) ||
              !accessesKnownIndependent(Ptr, StoreLoc, OtherPtr, OtherLoc, L,
                                        SE, AA)) {
            ConflictingAccess = true;
            break;
          }
        }
      }
      if (ConflictingAccess)
        break;
    }
    if (ConflictingAccess)
      continue;

    IRBuilder<> PreBuilder(Preheader->getTerminator());
    Value *InitValue =
        findStoredPreheaderValue(Ptr, Group.ValueType, Preheader, AA);
    if (!InitValue) {
      LoadInst *InitLoad = PreBuilder.CreateLoad(
          Group.ValueType, Ptr,
          Ptr->hasName() ? Ptr->getName() + ".morpher.reduc.init"
                         : "morpher.reduc.init");
      InitValue = InitLoad;
    }

    Instruction *PhiInsertPt = &*Header->getFirstInsertionPt();
    PHINode *AccPhi = PHINode::Create(Group.ValueType, 2, "morpher.reduc.acc", PhiInsertPt);
    AccPhi->addIncoming(InitValue, Preheader);

    LoopLoad->replaceAllUsesWith(AccPhi);

    Value *StoreValue = LoopStore->getValueOperand();
    if (StoreValue == AccPhi)
      continue;
    AccPhi->addIncoming(StoreValue, Latch);

    Instruction *ExitInsertPt = &*ExitBlock->getFirstInsertionPt();
    IRBuilder<> ExitBuilder(ExitInsertPt);
    ExitBuilder.CreateStore(StoreValue, Ptr);

    LoopStore->eraseFromParent();
    LoopLoad->eraseFromParent();

    errs() << "Morpher memory reduction scalarization: ptr="
           << Ptr->getName() << " loop_header=" << Header->getName() << "\n";
    Changed = true;
  }

  return Changed;
}

bool loopStoresMayAliasLoad(Loop *L, LoadInst *LI, AAResults &AA) {
  MemoryLocation LoadLoc = MemoryLocation::get(LI);
  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI)
        continue;
      if (SI->isVolatile() || SI->isAtomic())
        return true;
      if (AA.alias(LoadLoc, MemoryLocation::get(SI)) != AliasResult::NoAlias)
        return true;
    }
  }
  return false;
}

Value *materializeInvariantValueInPreheader(
    Value *V, Loop *L, AAResults &AA, IRBuilder<> &Builder,
    std::map<Value *, Value *> &Materialized,
    SmallPtrSetImpl<Value *> &Active) {
  if (Materialized.count(V))
    return Materialized[V];

  auto *I = dyn_cast<Instruction>(V);
  if (!I || !L->contains(I->getParent())) {
    Materialized[V] = V;
    return V;
  }

  if (Active.contains(V))
    return nullptr;
  Active.insert(V);

  Value *Result = nullptr;
  if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
    Value *Base = materializeInvariantValueInPreheader(
        GEP->getPointerOperand(), L, AA, Builder, Materialized, Active);
    if (Base) {
      SmallVector<Value *, 4> Indices;
      bool AllIndicesAvailable = true;
      for (Value *Idx : GEP->indices()) {
        Value *PreIdx = materializeInvariantValueInPreheader(
            Idx, L, AA, Builder, Materialized, Active);
        if (!PreIdx) {
          AllIndicesAvailable = false;
          break;
        }
        Indices.push_back(PreIdx);
      }
      if (AllIndicesAvailable)
        Result = Builder.CreateInBoundsGEP(
            GEP->getSourceElementType(), Base, Indices,
            GEP->hasName() ? GEP->getName() + ".morpher.hoist"
                           : "morpher.hoist.gep");
    }
  } else if (auto *BC = dyn_cast<BitCastInst>(I)) {
    Value *Op = materializeInvariantValueInPreheader(
        BC->getOperand(0), L, AA, Builder, Materialized, Active);
    if (Op)
      Result = Builder.CreateBitCast(Op, BC->getType(),
                                     BC->hasName() ? BC->getName() + ".morpher.hoist"
                                                   : "morpher.hoist.bitcast");
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    if (!LI->isVolatile() && !LI->isAtomic() &&
        !loopStoresMayAliasLoad(L, LI, AA)) {
      Value *Ptr = materializeInvariantValueInPreheader(
          LI->getPointerOperand(), L, AA, Builder, Materialized, Active);
      if (Ptr) {
        auto *Hoisted = Builder.CreateLoad(
            LI->getType(), Ptr,
            LI->hasName() ? LI->getName() + ".morpher.hoist"
                          : "morpher.hoist.load");
        Hoisted->setAlignment(LI->getAlign());
        Result = Hoisted;
      }
    }
  } else if (auto *BO = dyn_cast<BinaryOperator>(I)) {
    Value *LHS = materializeInvariantValueInPreheader(
        BO->getOperand(0), L, AA, Builder, Materialized, Active);
    Value *RHS = materializeInvariantValueInPreheader(
        BO->getOperand(1), L, AA, Builder, Materialized, Active);
    if (LHS && RHS)
      Result = Builder.CreateBinOp(
          BO->getOpcode(), LHS, RHS,
          BO->hasName() ? BO->getName() + ".morpher.hoist"
                        : "morpher.hoist.binop");
  } else if (auto *CI = dyn_cast<CastInst>(I)) {
    Value *Op = materializeInvariantValueInPreheader(
        CI->getOperand(0), L, AA, Builder, Materialized, Active);
    if (Op)
      Result = Builder.CreateCast(
          CI->getOpcode(), Op, CI->getType(),
          CI->hasName() ? CI->getName() + ".morpher.hoist"
                        : "morpher.hoist.cast");
  }

  Active.erase(V);
  if (Result)
    Materialized[V] = Result;
  return Result;
}

bool hoistLoopInvariantLoadsForUnroll(Loop *L, AAResults &AA) {
  BasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader)
    return false;

  SmallVector<LoadInst *, 16> Loads;
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      if (auto *LI = dyn_cast<LoadInst>(&I))
        Loads.push_back(LI);

  if (Loads.empty())
    return false;

  bool Changed = false;
  IRBuilder<> Builder(Preheader->getTerminator());
  std::map<Value *, Value *> Materialized;
  for (LoadInst *LI : Loads) {
    if (!LI || LI->getParent() == nullptr)
      continue;
    if (LI->isVolatile() || LI->isAtomic())
      continue;
    if (!LI->getType()->isIntegerTy() && !LI->getType()->isPointerTy())
      continue;
    if (loopStoresMayAliasLoad(L, LI, AA))
      continue;

    SmallPtrSet<Value *, 16> Active;
    Value *PrePtr = materializeInvariantValueInPreheader(
        LI->getPointerOperand(), L, AA, Builder, Materialized, Active);
    if (!PrePtr)
      continue;

    auto *Hoisted = Builder.CreateLoad(
        LI->getType(), PrePtr,
        LI->hasName() ? LI->getName() + ".morpher.inv"
                      : "morpher.inv.load");
    Hoisted->setAlignment(LI->getAlign());
    LI->replaceAllUsesWith(Hoisted);
    LI->eraseFromParent();
    Changed = true;
  }

  if (Changed)
    errs() << "Morpher invariant load hoist: function="
           << L->getHeader()->getParent()->getName()
           << " loop_header=" << L->getHeader()->getName() << "\n";
  return Changed;
}

void requireUnrollFactorSmallerThanKnownTrip(Loop *L, ScalarEvolution &SE,
                                             unsigned Factor,
                                             StringRef LoopName) {
  unsigned TripCount = SE.getSmallConstantTripCount(L);
  if (TripCount != 0 && Factor >= TripCount) {
    report_fatal_error("Morpher " + LoopName +
                       " unroll factor must be smaller than the known loop trip count");
  }

  unsigned MaxTripCount = SE.getSmallConstantMaxTripCount(L);
  if (MaxTripCount != 0 && Factor >= MaxTripCount) {
    report_fatal_error("Morpher " + LoopName +
                       " unroll factor must be smaller than the known max loop trip count");
  }
}

void canonicalizeLoopForUnroll(Loop *L, LoopInfo &LI, ScalarEvolution &SE,
                               DominatorTree &DT, AssumptionCache &AC) {
  // Unroll utilities with PreserveLCSSA=true assume simplified / LCSSA-friendly
  // input. Some benchmark IR carries direct live-outs, so canonicalize locally.
  simplifyLoop(L, &DT, &LI, &SE, &AC, nullptr, false);
  formLCSSARecursively(*L, DT, &LI, &SE);
}

bool unrollAnnotatedLoop(Loop *TargetLoop, LoopInfo &LI, ScalarEvolution &SE,
                         DominatorTree &DT, AssumptionCache &AC) {
  if (MorpherUnrollFactor <= 1)
    return false;

  canonicalizeLoopForUnroll(TargetLoop, LI, SE, DT, AC);
  requireUnrollFactorSmallerThanKnownTrip(TargetLoop, SE, MorpherUnrollFactor,
                                          "inner");

  annotateLoopInstructionsForSIMD(TargetLoop, MorpherUnrollFactor);

  UnrollLoopOptions ULO;
  ULO.Count = MorpherUnrollFactor;
  ULO.Force = true;
  ULO.Runtime = true;
  ULO.AllowExpensiveTripCount = true;
  ULO.UnrollRemainder = false;
  ULO.ForgetAllSCEV = true;

  Loop *RemainderLoop = nullptr;
  LoopUnrollResult Result =
      UnrollLoop(TargetLoop, ULO, &LI, &SE, &DT, &AC, nullptr, nullptr, true,
                 &RemainderLoop);

  if (Result == LoopUnrollResult::Unmodified)
    report_fatal_error("LLVM could not unroll the Morpher annotated loop");

  if (Result == LoopUnrollResult::FullyUnrolled)
    report_fatal_error("Morpher annotated loop was fully unrolled away");

  // If LLVM created a remainder loop (runtime peel for (TC % U) leftover
  // iterations), re-tag its markers as `please_map_me_peel` so dfggenPass can
  // optionally emit a peel DFG (entry point for end-to-end loop-peel P/E; see
  // PLAN.md). The retagging is opt-in via -morpher-emit-peel; when disabled
  // we instead delete the remainder marker (matching legacy behavior where
  // `normalizePleaseMapMeMarkers` erased the orphan). This keeps the legacy
  // single-kernel-DFG pipeline working until Phase 2b is fully wired up,
  // since today's `DFG::nameNodes` asserts on i64 OutLoopSTORE produced when
  // dfggenPass sees the kernel's live-out IV reach the peel preheader.
  if (RemainderLoop) {
    if (MorpherEmitPeel) {
      unsigned retagged = retagRemainderMarkersAsPeel(RemainderLoop);
      errs() << "Morpher peel: remainder loop header="
             << RemainderLoop->getHeader()->getName()
             << " retagged_markers=" << retagged << "\n";
    } else {
      // Mirror the legacy normalize-erase behavior: delete `please_map_me()`
      // calls inside the remainder so downstream tooling sees a clean IR.
      unsigned erased = 0;
      SmallVector<CallBase *, 4> toErase;
      for (BasicBlock *BB : RemainderLoop->blocks())
        for (Instruction &I : *BB)
          if (auto *CB = dyn_cast<CallBase>(&I))
            if (isPleaseMapMe(CB))
              toErase.push_back(CB);
      for (CallBase *CB : toErase) {
        CB->eraseFromParent();
        ++erased;
      }
      if (erased > 0) {
        errs() << "Morpher peel: erased " << erased
               << " remainder marker(s) (peel emission disabled)\n";
      }
    }
  }

  formLCSSARecursively(*TargetLoop, DT, &LI, &SE);
  return true;
}

ConstantInt *getConstantIncoming(PHINode *Phi, BasicBlock *IncomingBlock) {
  int Idx = Phi->getBasicBlockIndex(IncomingBlock);
  if (Idx < 0)
    return nullptr;
  return dyn_cast<ConstantInt>(Phi->getIncomingValue(Idx));
}

bool blockHasOnlyAllowedPhi(BasicBlock *BB, PHINode *Allowed) {
  for (PHINode &Phi : BB->phis())
    if (&Phi != Allowed)
      return false;
  return true;
}

bool valuesInBlockDoNotEscapeExceptPhi(BasicBlock *BB, PHINode *AllowedPhi) {
  for (Instruction &I : *BB) {
    if (I.isTerminator())
      continue;
    for (User *U : I.users()) {
      auto *UserI = dyn_cast<Instruction>(U);
      if (UserI && UserI == AllowedPhi)
        continue;
      if (!UserI || UserI->getParent() != BB)
        return false;
    }
  }
  return true;
}

bool hasInvariantAddressLoadStorePair(Loop *L) {
  std::map<Value *, bool> HasLoad;
  std::map<Value *, bool> HasStore;
  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->isVolatile() || LI->isAtomic())
          continue;
        Value *Ptr = LI->getPointerOperand();
        if (L->isLoopInvariant(Ptr))
          HasLoad[Ptr] = true;
        continue;
      }
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (SI->isVolatile() || SI->isAtomic())
          continue;
        Value *Ptr = SI->getPointerOperand();
        if (L->isLoopInvariant(Ptr))
          HasStore[Ptr] = true;
      }
    }
  }

  for (const auto &Entry : HasLoad)
    if (Entry.second && HasStore[Entry.first])
      return true;
  return false;
}

void replacePhiIncomingBlock(BasicBlock *BB, BasicBlock *OldBlock,
                             BasicBlock *NewBlock) {
  for (PHINode &Phi : BB->phis()) {
    for (unsigned I = 0; I < Phi.getNumIncomingValues(); ++I)
      if (Phi.getIncomingBlock(I) == OldBlock)
        Phi.setIncomingBlock(I, NewBlock);
  }
}

bool flattenAnnotatedInnermostLoop(Function &F, LoopInfo &LI,
                                   ScalarEvolution &SE) {
  Loop *InnerLoop = nullptr;
  findAnnotatedLoopMarker(F, LI, InnerLoop);
  if (!InnerLoop)
    return false;
  if (loopCarriesSystolicCandidate(InnerLoop)) {
    errs() << "Morpher loop fusion: skipping function=" << F.getName()
           << " loop_header=" << InnerLoop->getHeader()->getName()
           << " reason=systolic_candidate\n";
    return false;
  }
  if (!InnerLoop->getSubLoops().empty()) {
    errs() << "Morpher loop fusion skipped: annotated loop is not innermost\n";
    return false;
  }

  Loop *OuterLoop = InnerLoop->getParentLoop();
  if (!OuterLoop) {
    errs() << "Morpher loop fusion skipped: annotated loop has no direct parent\n";
    return false;
  }
  if (OuterLoop->getSubLoops().size() != 1 ||
      OuterLoop->getSubLoops().front() != InnerLoop) {
    errs() << "Morpher loop fusion skipped: parent loop is not a perfect one-child nest\n";
    return false;
  }
  if (InnerLoop->getBlocks().size() != 1) {
    errs() << "Morpher loop fusion skipped: inner loop is not single-block canonical\n";
    return false;
  }

  BasicBlock *OuterHeader = OuterLoop->getHeader();
  BasicBlock *OuterPreheader = OuterLoop->getLoopPreheader();
  BasicBlock *OuterLatch = OuterLoop->getLoopLatch();
  BasicBlock *OuterExit = OuterLoop->getExitBlock();
  BasicBlock *InnerHeader = InnerLoop->getHeader();
  BasicBlock *InnerPreheader = InnerLoop->getLoopPreheader();
  BasicBlock *InnerLatch = InnerLoop->getLoopLatch();
  BasicBlock *InnerExit = InnerLoop->getExitBlock();

  if (!OuterHeader || !OuterPreheader || !OuterLatch || !OuterExit ||
      !InnerHeader || !InnerPreheader || !InnerLatch || !InnerExit) {
    errs() << "Morpher loop fusion skipped: missing canonical loop blocks\n";
    return false;
  }
  if (InnerHeader != InnerLatch || InnerPreheader != OuterHeader ||
      InnerExit != OuterLatch) {
    errs() << "Morpher loop fusion skipped: unsupported parent/inner CFG shape\n";
    return false;
  }
  if (hasInvariantAddressLoadStorePair(InnerLoop)) {
    errs() << "Morpher loop fusion skipped: inner-loop invariant memory recurrence "
           << "must remain on the original reduction loop\n";
    return false;
  }
  unsigned OuterTrip = SE.getSmallConstantTripCount(OuterLoop);
  unsigned InnerTrip = SE.getSmallConstantTripCount(InnerLoop);
  if (OuterTrip == 0 || InnerTrip == 0) {
    errs() << "Morpher loop fusion skipped: parent/inner trip count is not a known constant\n";
    return false;
  }

  PHINode *OuterIV = OuterLoop->getInductionVariable(SE);
  PHINode *InnerIV = InnerLoop->getInductionVariable(SE);
  if (!OuterIV || !InnerIV || !OuterIV->getType()->isIntegerTy() ||
      OuterIV->getType() != InnerIV->getType()) {
    errs() << "Morpher loop fusion skipped: could not identify compatible induction variables\n";
    return false;
  }
  if (!valuesInBlockDoNotEscapeExceptPhi(OuterLatch, OuterIV)) {
    errs() << "Morpher loop fusion skipped: parent latch values escape beyond the parent IV\n";
    return false;
  }
  if (!blockHasOnlyAllowedPhi(OuterHeader, OuterIV) ||
      !blockHasOnlyAllowedPhi(InnerHeader, InnerIV)) {
    errs() << "Morpher loop fusion skipped: extra loop header PHIs are unsupported\n";
    return false;
  }
  ConstantInt *OuterStart = getConstantIncoming(OuterIV, OuterPreheader);
  ConstantInt *InnerStart = getConstantIncoming(InnerIV, OuterHeader);
  if (!OuterStart || !InnerStart) {
    errs() << "Morpher loop fusion skipped: induction variables do not have constant starts\n";
    return false;
  }

  uint64_t Product64 = uint64_t(OuterTrip) * uint64_t(InnerTrip);
  Type *IVTy = OuterIV->getType();
  unsigned BitWidth = IVTy->getIntegerBitWidth();
  if (Product64 >= (BitWidth >= 64 ? UINT64_MAX : (uint64_t(1) << BitWidth))) {
    errs() << "Morpher loop fusion skipped: flattened trip count overflows IV type\n";
    return false;
  }

  PHINode *FlatIV = PHINode::Create(IVTy, 2, "morpher.flat.iv",
                                    &*OuterHeader->begin());
  FlatIV->addIncoming(ConstantInt::get(IVTy, 0), OuterPreheader);

  IRBuilder<> HeaderBuilder(&*OuterHeader->getFirstInsertionPt());
  Value *InnerTripC = ConstantInt::get(IVTy, InnerTrip);
  Value *OuterQuot = HeaderBuilder.CreateUDiv(FlatIV, InnerTripC,
                                              "morpher.flat.outer.quot");
  Value *OuterScaled = HeaderBuilder.CreateMul(OuterQuot, InnerTripC,
                                               "morpher.flat.outer.scaled");
  Value *InnerOffset = HeaderBuilder.CreateSub(FlatIV, OuterScaled,
                                               "morpher.flat.inner.offset");
  Value *OuterValue = OuterStart->isZero()
                          ? OuterQuot
                          : HeaderBuilder.CreateAdd(OuterQuot, OuterStart,
                                                    "morpher.flat.outer");
  Value *InnerValue = InnerStart->isZero()
                          ? InnerOffset
                          : HeaderBuilder.CreateAdd(InnerOffset, InnerStart,
                                                    "morpher.flat.inner");

  OuterIV->replaceAllUsesWith(OuterValue);
  InnerIV->replaceAllUsesWith(InnerValue);
  if (OuterIV->use_empty())
    OuterIV->eraseFromParent();
  if (InnerIV->use_empty())
    InnerIV->eraseFromParent();

  BranchInst *OldInnerBranch = dyn_cast<BranchInst>(InnerHeader->getTerminator());
  if (!OldInnerBranch || !OldInnerBranch->isConditional()) {
    errs() << "Morpher loop fusion skipped: inner latch is not conditional\n";
    return false;
  }

  IRBuilder<> LatchBuilder(OldInnerBranch);
  Value *FlatNext = LatchBuilder.CreateAdd(
      FlatIV, ConstantInt::get(IVTy, 1), "morpher.flat.next");
  Value *Done = LatchBuilder.CreateICmpEQ(
      FlatNext, ConstantInt::get(IVTy, Product64), "morpher.flat.done");
  FlatIV->addIncoming(FlatNext, InnerHeader);

  replacePhiIncomingBlock(OuterExit, OuterLatch, InnerHeader);
  OldInnerBranch->eraseFromParent();
  BranchInst::Create(OuterExit, OuterHeader, Done, InnerHeader);

  Instruction *OuterLatchTerm = OuterLatch->getTerminator();
  OuterLatchTerm->eraseFromParent();
  IRBuilder<> DeadLatchBuilder(OuterLatch);
  DeadLatchBuilder.CreateUnreachable();

  errs() << "Morpher loop fusion: function=" << F.getName()
         << " outer_trip=" << OuterTrip
         << " inner_trip=" << InnerTrip
         << " flattened_trip=" << Product64 << "\n";
  return true;
}

static bool runMorpherLoopUnroll(Function &F, LoopInfo &LI, ScalarEvolution &SE,
                                 DominatorTree &DT, AssumptionCache &AC,
                                 AAResults &AA) {
    if (MorpherUnrollFactor <= 1)
      return false;

    if (!MorpherUnrollFn.empty() && F.getName() != MorpherUnrollFn)
      return false;

    if (MorpherReductionPolicy != "tree" &&
        MorpherReductionPolicy != "packed-psum" &&
        MorpherReductionPolicy != "scalar-psum" &&
        MorpherReductionPolicy != "vector-psum") {
      report_fatal_error("morpher-reduction-policy must be tree, packed-psum, scalar-psum, or vector-psum");
    }

    Loop *TargetLoop = nullptr;
    findAnnotatedLoopMarker(F, LI, TargetLoop);
    if (TargetLoop && loopCarriesSystolicCandidate(TargetLoop)) {
      errs() << "Morpher loop unroll: skipping function=" << F.getName()
             << " loop_header=" << TargetLoop->getHeader()->getName()
             << " reason=systolic_candidate\n";
      return false;
    }
    bool Changed = false;
    canonicalizeLoopForUnroll(TargetLoop, LI, SE, DT, AC);
    if (MorpherPromoteMemoryReductions)
      Changed |= scalarizeInvariantAddressReductionForLoop(TargetLoop, SE, DT, AA);
    Changed |= hoistLoopInvariantLoadsForUnroll(TargetLoop, AA);
    Changed |= unrollAnnotatedLoop(TargetLoop, LI, SE, DT, AC);

    Changed |= expandReductionsForSIMD(TargetLoop, MorpherReductionLanes,
                                       MorpherReductionPolicy);
    splitUnrolledSIMDGroups(TargetLoop, MorpherReductionLanes);
    normalizePleaseMapMeMarkers(F, LI, TargetLoop);

    errs() << "Morpher loop unroll: function=" << F.getName()
           << " factor=" << MorpherUnrollFactor
           << " fuse_innermost_loops="
           << (MorpherFuseInnermostLoops ? "true" : "false")
           << " remainder_policy=preserve"
           << " reduction_lanes=" << MorpherReductionLanes
           << " promote_memory_reductions="
           << (MorpherPromoteMemoryReductions ? "true" : "false")
           << " reduction_policy=" << MorpherReductionPolicy << "\n";
    return Changed;
  }
struct MorpherLoopUnrollPass : public PassInfoMixin<MorpherLoopUnrollPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);
    AssumptionCache &AC = AM.getResult<AssumptionAnalysis>(F);
    AAResults &AA = AM.getResult<AAManager>(F);

    return runMorpherLoopUnroll(F, LI, SE, DT, AC, AA)
               ? PreservedAnalyses::none()
               : PreservedAnalyses::all();
  }
};

struct MorpherLoopFlattenPass : public PassInfoMixin<MorpherLoopFlattenPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    if (!MorpherFuseInnermostLoops)
      return PreservedAnalyses::all();
    if (!MorpherUnrollFn.empty() && F.getName() != MorpherUnrollFn)
      return PreservedAnalyses::all();

    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    return flattenAnnotatedInnermostLoop(F, LI, SE) ? PreservedAnalyses::none()
                                                    : PreservedAnalyses::all();
  }
};

} // namespace

FunctionPassManager createMorpherLoopUnrollPipeline() {
  FunctionPassManager FPM;
  if (MorpherFuseInnermostLoops)
    FPM.addPass(MorpherLoopFlattenPass());
  FPM.addPass(MorpherLoopUnrollPass());
  return FPM;
}
