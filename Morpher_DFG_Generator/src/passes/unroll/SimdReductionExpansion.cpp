#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

#include "UnrollInternal.h"

using namespace llvm;
using namespace morpher_unroll;

namespace {

void setStringMetadata(Instruction &I, StringRef Name, StringRef Value) {
  LLVMContext &Ctx = I.getContext();
  Metadata *MD = MDString::get(Ctx, Value);
  I.setMetadata(Name, MDNode::get(Ctx, MD));
}

StringRef reductionOpcodeName(unsigned Opcode) {
  switch (Opcode) {
  case Instruction::Add:
    return "ADD";
  case Instruction::Sub:
    return "SUB";
  case Instruction::Mul:
    return "MUL";
  case Instruction::And:
    return "AND";
  case Instruction::Or:
    return "OR";
  case Instruction::Xor:
    return "XOR";
  default:
    return "UNKNOWN";
  }
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

    // A chain link is the PHI itself or another op with the same reduction
    // opcode. Using isa<BinaryOperator> alone would incorrectly match e.g.
    // a MUL on the non-chain operand (MUL is also a BinaryOperator).
    auto isChainLink = [&](Value *V) -> bool {
      if (V == Phi) return true;
      if (auto *VBO = dyn_cast<BinaryOperator>(V))
        return VBO->getOpcode() == ReductionOpcode;
      return false;
    };

    if (ReductionOpcode == Instruction::Sub) {
      if (isChainLink(LHS)) {
        Prev = LHS;
        Term = RHS;
      } else {
        return false;
      }
    } else if (isChainLink(LHS)) {
      Prev = LHS;
      Term = RHS;
    } else if (isChainLink(RHS)) {
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
    // Binary tree reduction: pair up terms level by level.
    // Depth = ceil(log2(Lanes)), vs. Lanes-1 for a linear chain.
    SmallVector<Value *, 16> Level(Terms.begin() + Start, Terms.begin() + Start + Lanes);
    while (Level.size() > 1) {
      SmallVector<Value *, 16> Next;
      for (unsigned I = 0; I + 1 < Level.size(); I += 2)
        Next.push_back(createReductionOp(BodyBuilder, termCombineOpcode(ReductionOpcode),
                                         Level[I], Level[I + 1], "morpher.red.tree"));
      if (Level.size() & 1)
        Next.push_back(Level.back());
      Level = std::move(Next);
    }
    Value *Update = createReductionOp(BodyBuilder, ReductionOpcode, Partials[P],
                                      Level[0], "morpher.red.update");
    Updates.push_back(Update);
    Partials[P]->addIncoming(Update, Latch);
  }

  Value *Combined = Updates[0];
  for (unsigned I = 1; I < Updates.size(); ++I)
    Combined = createReductionOp(BodyBuilder, finalCombineOpcode(ReductionOpcode), Combined,
                                 Updates[I], "morpher.red.combine");

  BackedgeValue->replaceAllUsesWith(Combined);
  for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
    if ((*It)->use_empty())
      (*It)->eraseFromParent();

  unsigned TreeDepth = 0;
  for (unsigned N = Lanes; N > 1; N = (N + 1) / 2)
    ++TreeDepth;
  errs() << "Morpher reduction expansion (tree): phi=" << Phi->getName()
         << " lanes=" << Lanes
         << " terms=" << Terms.size()
         << " partials=" << PartialCount
         << " depth=" << TreeDepth << "\n";
  return true;
}

bool expandIntegerReductionPackedPsum(PHINode *Phi, Loop *L, unsigned Lanes) {
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

  Value *BackedgeValue = Phi->getIncomingValue(LatchIdx);
  if (!Phi->getType()->isIntegerTy())
    return false;
  std::string PhiName = Phi->getName().str();

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

  unsigned VectorGroups = Terms.size() / Lanes;

  SmallVector<Use *, 8> ExternalBackedgeUses;
  for (Use &U : BackedgeValue->uses()) {
    auto *UserI = dyn_cast<Instruction>(U.getUser());
    if (!UserI || !L->contains(UserI))
      ExternalBackedgeUses.push_back(&U);
  }

  unsigned NextGroup = 1000000;
  SmallVector<SmallVector<Value *, 16>, 16> LaneLevels;
  LaneLevels.resize(Lanes);
  for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
    for (unsigned Group = 0; Group < VectorGroups; ++Group)
      LaneLevels[Lane].push_back(Terms[Group * Lanes + Lane]);
  }

  IRBuilder<> BodyBuilder(Chain.back());
  while (LaneLevels[0].size() > 1) {
    SmallVector<SmallVector<Value *, 16>, 16> NextLevels;
    NextLevels.resize(Lanes);
    for (unsigned Pair = 0; Pair + 1 < LaneLevels[0].size(); Pair += 2) {
      unsigned TreeGroup = NextGroup++;
      for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
        Value *Combined = createReductionOp(
            BodyBuilder, termCombineOpcode(ReductionOpcode),
            LaneLevels[Lane][Pair], LaneLevels[Lane][Pair + 1],
            "morpher.psum.tree");
        if (auto *I = dyn_cast<Instruction>(Combined))
          setSIMDMetadata(*I, TreeGroup, Lanes);
        NextLevels[Lane].push_back(Combined);
      }
    }
    if (LaneLevels[0].size() & 1) {
      unsigned Last = LaneLevels[0].size() - 1;
      for (unsigned Lane = 0; Lane < Lanes; ++Lane)
        NextLevels[Lane].push_back(LaneLevels[Lane][Last]);
    }
    LaneLevels = std::move(NextLevels);
  }

  SmallVector<Value *, 16> LaneOutputs;
  for (unsigned Lane = 0; Lane < Lanes; ++Lane)
    LaneOutputs.push_back(LaneLevels[Lane][0]);

  Function *F = Phi->getFunction();
  LLVMContext &Ctx = F->getContext();
  IRBuilder<> EntryBuilder(&*F->getEntryBlock().getFirstInsertionPt());
  Value *LaneCount = ConstantInt::get(Type::getInt32Ty(Ctx), Lanes);
  AllocaInst *VectorOut =
      EntryBuilder.CreateAlloca(Phi->getType(), LaneCount,
                                PhiName + ".morpher.vecout");
  unsigned StoreGroup = NextGroup++;
  SmallVector<StoreInst *, 16> VectorStores;
  for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
    Value *LaneIdx = ConstantInt::get(Type::getInt32Ty(Ctx), Lane);
    Value *Slot = BodyBuilder.CreateInBoundsGEP(Phi->getType(), VectorOut,
                                                LaneIdx,
                                                "morpher.vecout.ptr");
    StoreInst *Store = BodyBuilder.CreateStore(LaneOutputs[Lane], Slot);
    Store->setVolatile(true);
    setSIMDMetadata(*Store, StoreGroup, Lanes);
    VectorStores.push_back(Store);
  }

  IRBuilder<> ExitBuilder(Latch->getTerminator());
  Value *Combined = LaneOutputs[0];
  for (unsigned Lane = 1; Lane < LaneOutputs.size(); ++Lane)
    Combined = createReductionOp(ExitBuilder, finalCombineOpcode(ReductionOpcode),
                                 Combined, LaneOutputs[Lane], "morpher.psum.final");
  // The unrolled lane terms replace one original reduction step. Preserve the
  // loop-carried accumulator by combining the horizontal lane sum with the
  // PHI, not its preheader initializer on every unrolled iteration.
  Combined = createReductionOp(ExitBuilder, finalCombineOpcode(ReductionOpcode),
                               Phi, Combined, "morpher.psum.final.init");
  StringRef ReduceOpName = reductionOpcodeName(ReductionOpcode);
  StringRef ReduceBaseName = Combined->hasName() ? Combined->getName() : "morpher.psum.final.init";
  for (StoreInst *Store : VectorStores) {
    setStringMetadata(*Store, "morpher.simd.reduce_op", ReduceOpName);
    setStringMetadata(*Store, "morpher.simd.reduce_base", ReduceBaseName);
  }

  for (Use *U : ExternalBackedgeUses)
    U->set(Combined);

  Phi->setIncomingValue(LatchIdx, Combined);
  for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
    if ((*It)->use_empty())
      (*It)->eraseFromParent();
  if (Phi->use_empty())
    Phi->eraseFromParent();

  errs() << "Morpher reduction expansion (packed-output): phi=" << PhiName
         << " lanes=" << Lanes
         << " terms=" << Terms.size()
         << " vector_groups=" << VectorGroups
         << " deferred_horizontal=1\n";
  return true;
}

bool expandIntegerReductionScalarPsum(PHINode *Phi, Loop *L, unsigned Lanes) {
  if (Lanes <= 1)
    return false;

  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Latch = L->getLoopLatch();
  BasicBlock *Exit = L->getExitBlock();
  if (!Preheader || !Latch || !Exit)
    return false;
  if (Exit->getSinglePredecessor() != Latch)
    return false;

  int PreheaderIdx = Phi->getBasicBlockIndex(Preheader);
  int LatchIdx = Phi->getBasicBlockIndex(Latch);
  if (PreheaderIdx < 0 || LatchIdx < 0)
    return false;

  Value *Init = Phi->getIncomingValue(PreheaderIdx);
  Value *BackedgeValue = Phi->getIncomingValue(LatchIdx);
  if (!Phi->getType()->isIntegerTy())
    return false;
  std::string PhiName = Phi->getName().str();

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

  SmallVector<PHINode *, 8> ExternalLCSSAPhis;
  SmallVector<Use *, 8> DirectExternalUses;
  for (Use &U : BackedgeValue->uses()) {
    auto *UserI = dyn_cast<Instruction>(U.getUser());
    if (UserI && L->contains(UserI))
      continue;
    if (auto *UserPhi = dyn_cast_or_null<PHINode>(UserI)) {
      if (UserPhi->getParent() == Exit) {
        ExternalLCSSAPhis.push_back(UserPhi);
        continue;
      }
    }
    DirectExternalUses.push_back(&U);
  }
  if (!DirectExternalUses.empty())
    return false;
  if (ExternalLCSSAPhis.empty())
    return false;

  unsigned PartialCount = Terms.size() / Lanes;
  Value *Identity = identityForReduction(ReductionOpcode, Phi->getType());
  if (!Identity)
    return false;

  IRBuilder<> PhiBuilder(&*Phi->getParent()->getFirstInsertionPt());
  SmallVector<PHINode *, 16> Partials;
  for (unsigned I = 0; I < PartialCount; ++I) {
    PHINode *Partial = PhiBuilder.CreatePHI(Phi->getType(), 2,
                                            Phi->getName() + ".morpher.spsum");
    Partial->addIncoming(I == 0 ? Init : Identity, Preheader);
    Partials.push_back(Partial);
  }

  IRBuilder<> BodyBuilder(Chain.back());
  SmallVector<Value *, 16> Updates;
  for (unsigned P = 0; P < PartialCount; ++P) {
    unsigned Start = P * Lanes;
    SmallVector<Value *, 16> Level(Terms.begin() + Start,
                                   Terms.begin() + Start + Lanes);
    while (Level.size() > 1) {
      SmallVector<Value *, 16> Next;
      for (unsigned I = 0; I + 1 < Level.size(); I += 2)
        Next.push_back(createReductionOp(BodyBuilder,
                                         termCombineOpcode(ReductionOpcode),
                                         Level[I], Level[I + 1],
                                         "morpher.spsum.tree"));
      if (Level.size() & 1)
        Next.push_back(Level.back());
      Level = std::move(Next);
    }
    Value *Update = createReductionOp(BodyBuilder, ReductionOpcode, Partials[P],
                                      Level[0], "morpher.spsum.update");
    Updates.push_back(Update);
    Partials[P]->addIncoming(Update, Latch);
  }

  IRBuilder<> ExitBuilder(&*Exit->getFirstInsertionPt());
  Value *Combined = Updates[0];
  for (unsigned I = 1; I < Updates.size(); ++I)
    Combined = createReductionOp(ExitBuilder, finalCombineOpcode(ReductionOpcode),
                                 Combined, Updates[I],
                                 "morpher.spsum.final");

  for (PHINode *LCSSAPhi : ExternalLCSSAPhis) {
    LCSSAPhi->replaceAllUsesWith(Combined);
    if (LCSSAPhi->use_empty())
      LCSSAPhi->eraseFromParent();
  }

  for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
    if ((*It)->use_empty())
      (*It)->eraseFromParent();
  if (Phi->use_empty())
    Phi->eraseFromParent();

  unsigned TreeDepth = 0;
  for (unsigned N = Lanes; N > 1; N = (N + 1) / 2)
    ++TreeDepth;
  errs() << "Morpher reduction expansion (scalar-psum): phi="
         << PhiName
         << " lanes=" << Lanes
         << " terms=" << Terms.size()
         << " partials=" << PartialCount
         << " local_depth=" << TreeDepth
         << " horizontal_outside_loop=1\n";
  return true;
}

bool expandIntegerReductionVectorPsum(PHINode *Phi, Loop *L,
                                      unsigned Lanes, unsigned &NextGroup) {
  if (Lanes <= 1)
    return false;

  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Latch = L->getLoopLatch();
  BasicBlock *Exit = L->getExitBlock();
  if (!Preheader || !Latch || !Exit)
    return false;
  if (Exit->getSinglePredecessor() != Latch)
    return false;

  int PreheaderIdx = Phi->getBasicBlockIndex(Preheader);
  int LatchIdx = Phi->getBasicBlockIndex(Latch);
  if (PreheaderIdx < 0 || LatchIdx < 0 ||
      !Phi->getType()->isIntegerTy(32))
    return false;

  Value *Init = Phi->getIncomingValue(PreheaderIdx);
  Value *BackedgeValue = Phi->getIncomingValue(LatchIdx);
  std::string PhiName = Phi->getName().str();

  SmallVector<BinaryOperator *, 32> Chain;
  SmallVector<Value *, 32> Terms;
  unsigned ReductionOpcode = 0;
  if (!collectReductionAddChain(Phi, BackedgeValue, ReductionOpcode, Chain,
                                Terms))
    return false;

  // These are exactly the integer operations supported by the VRED lowering.
  // SUB and MUL need a different exit contract, and floating-point reductions
  // require explicit reassociation permission.
  if (ReductionOpcode != Instruction::Add &&
      ReductionOpcode != Instruction::And &&
      ReductionOpcode != Instruction::Or &&
      ReductionOpcode != Instruction::Xor)
    return false;

  bool HasNonConstantTerm = false;
  for (Value *Term : Terms)
    if (!isa<ConstantInt>(Term)) {
      HasNonConstantTerm = true;
      break;
    }
  if (!HasNonConstantTerm || Terms.size() < Lanes ||
      Terms.size() % Lanes != 0)
    return false;

  SmallVector<PHINode *, 8> ExternalLCSSAPhis;
  SmallVector<Use *, 8> DirectExternalUses;
  for (Use &U : BackedgeValue->uses()) {
    auto *UserI = dyn_cast<Instruction>(U.getUser());
    if (UserI && L->contains(UserI))
      continue;
    if (auto *UserPhi = dyn_cast_or_null<PHINode>(UserI)) {
      if (UserPhi->getParent() == Exit) {
        ExternalLCSSAPhis.push_back(UserPhi);
        continue;
      }
    }
    DirectExternalUses.push_back(&U);
  }
  if (!DirectExternalUses.empty() || ExternalLCSSAPhis.empty())
    return false;

  Value *Identity = identityForReduction(ReductionOpcode, Phi->getType());
  if (!Identity)
    return false;

  // Scalar lane PHIs preserve legal LLVM IR. The verified DFG pack recognizes
  // the annotated lane updates below, replaces their PHI/select/merge
  // scaffolding with one vector recurrence, and records the exit-only work in
  // its manifest.
  IRBuilder<> PhiBuilder(&*Phi->getParent()->getFirstInsertionPt());
  SmallVector<PHINode *, 16> LanePartials;
  for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
    PHINode *Partial = PhiBuilder.CreatePHI(
        Phi->getType(), 2, Phi->getName() + ".morpher.vpsum");
    Partial->addIncoming(Identity, Preheader);
    LanePartials.push_back(Partial);
  }

  IRBuilder<> BodyBuilder(Chain.back());
  StringRef ReduceOpName = reductionOpcodeName(ReductionOpcode);
  const unsigned VectorGroups = Terms.size() / Lanes;
  SmallVector<Value *, 16> LaneTerms(Terms.begin(), Terms.begin() + Lanes);
  for (unsigned Group = 1; Group < VectorGroups; ++Group) {
    const unsigned LocalGroup = NextGroup++;
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      Value *Combined = createReductionOp(
          BodyBuilder, ReductionOpcode, LaneTerms[Lane],
          Terms[Group * Lanes + Lane], "morpher.vpsum.local");
      setSIMDMetadata(*cast<Instruction>(Combined), LocalGroup, Lanes);
      LaneTerms[Lane] = Combined;
    }
  }

  const unsigned UpdateGroup = NextGroup++;
  const std::string InitTraceVar =
      "morpher.vpsum.init." + std::to_string(UpdateGroup);
  const std::string ExitTraceVar =
      "morpher.vpsum.exit." + std::to_string(UpdateGroup);
  SmallVector<std::string, 16> SeedTraceVars;
  for (unsigned Lane = 0; Lane < Lanes; ++Lane)
    SeedTraceVars.push_back(
        "morpher.vpsum.seed." + std::to_string(UpdateGroup) + "." +
        std::to_string(Lane));
  SmallVector<Value *, 16> Updates;
  for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
    Value *Update = createReductionOp(
        BodyBuilder, ReductionOpcode, LanePartials[Lane], LaneTerms[Lane],
        "morpher.vpsum.update");
    auto *UpdateI = cast<Instruction>(Update);
    setSIMDMetadata(*UpdateI, UpdateGroup, Lanes);
    setStringMetadata(*UpdateI, "morpher.simd.accumulator", ReduceOpName);
    setStringMetadata(*UpdateI, "morpher.simd.accumulator_init_var",
                      InitTraceVar);
    setStringMetadata(*UpdateI, "morpher.simd.accumulator_exit_var",
                      ExitTraceVar);
    setStringMetadata(*UpdateI, "morpher.simd.accumulator_seed_var",
                      SeedTraceVars[Lane]);
    LanePartials[Lane]->addIncoming(Update, Latch);
    Updates.push_back(Update);
  }

  // This executes once after the mapped loop. It deliberately consumes the
  // original initializer here rather than placing it on any lane recurrence.
  IRBuilder<> ExitBuilder(&*Exit->getFirstInsertionPt());
  Value *Horizontal = Updates[0];
  for (unsigned Lane = 1; Lane < Updates.size(); ++Lane)
    Horizontal = createReductionOp(
        ExitBuilder, ReductionOpcode, Horizontal, Updates[Lane],
        "morpher.vpsum.exit.reduce");
  Value *Final = createReductionOp(
      ExitBuilder, ReductionOpcode, Init, Horizontal,
      "morpher.vpsum.exit.init");

  Module *M = Phi->getModule();
  LLVMContext &Ctx = M->getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  FunctionType *TraceType = FunctionType::get(
      Type::getVoidTy(Ctx), {PtrTy, Type::getInt32Ty(Ctx)}, false);
  FunctionCallee TraceInit =
      M->getOrInsertFunction("MorpherVectorAccumulatorInit", TraceType);
  FunctionCallee TraceExit =
      M->getOrInsertFunction("MorpherVectorAccumulatorExit", TraceType);
  IRBuilder<> PreheaderTraceBuilder(Preheader->getTerminator());
  PreheaderTraceBuilder.CreateCall(
      TraceInit,
      {PreheaderTraceBuilder.CreateGlobalStringPtr(InitTraceVar), Init});
  for (const std::string &SeedTraceVar : SeedTraceVars)
    PreheaderTraceBuilder.CreateCall(
        TraceInit,
        {PreheaderTraceBuilder.CreateGlobalStringPtr(SeedTraceVar), Identity});
  ExitBuilder.CreateCall(
      TraceExit, {ExitBuilder.CreateGlobalStringPtr(ExitTraceVar), Final});

  for (PHINode *LCSSAPhi : ExternalLCSSAPhis) {
    LCSSAPhi->replaceAllUsesWith(Final);
    if (LCSSAPhi->use_empty())
      LCSSAPhi->eraseFromParent();
  }

  // Break the now-dead original scalar recurrence before erasing its chain.
  // Without this, Phi -> Chain -> Phi remains a use cycle even though no live
  // value consumes it, and the DFG extractor would retain both recurrences.
  Phi->setIncomingValue(LatchIdx, Init);
  for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
    if ((*It)->use_empty())
      (*It)->eraseFromParent();
  if (Phi->use_empty())
    Phi->eraseFromParent();

  errs() << "Morpher reduction expansion (vector-psum): phi=" << PhiName
         << " lanes=" << Lanes
         << " terms=" << Terms.size()
         << " vector_groups=" << VectorGroups
         << " vector_recurrence=1 exit_horizontal=1\n";
  return true;
}

} // namespace

namespace morpher_unroll {

bool expandReductionsForSIMD(Loop *L, unsigned Lanes, StringRef Policy) {
  if (Lanes <= 1)
    return false;

  SmallVector<PHINode *, 8> Phis;
  for (PHINode &Phi : L->getHeader()->phis())
    Phis.push_back(&Phi);

  bool Changed = false;
  unsigned VectorAccumulatorGroup = 2000000;
  for (PHINode *Phi : Phis) {
    if (Policy == "packed-psum")
      Changed |= expandIntegerReductionPackedPsum(Phi, L, Lanes);
    else if (Policy == "scalar-psum")
      Changed |= expandIntegerReductionScalarPsum(Phi, L, Lanes);
    else if (Policy == "vector-psum")
      Changed |= expandIntegerReductionVectorPsum(
          Phi, L, Lanes, VectorAccumulatorGroup);
    else
      Changed |= expandIntegerAddReduction(Phi, L, Lanes);
  }
  return Changed;
}

} // namespace morpher_unroll
