// Systolic loop-nest discovery and legality analysis. This is mechanical
// code motion from DetectPass.cpp; the detector remains the policy/report driver.

#include "SystolicInternal.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IntrinsicInst.h"

#include <limits>
#include <optional>
#include <string>

using namespace llvm;

namespace morpher_systolic {

// ----------------------------------------------------------------------------
// Phase 1: discover perfect single-subloop nests
// ----------------------------------------------------------------------------

// Phase 3 relaxation: enumerate every innermost loop and walk up to its
// outermost ancestor. This finds nest candidates even when outer levels have
// multiple subloop children (e.g., 2mm's shared outer loop hosting two
// sibling inner contractions). A single loop is also a valid candidate for
// 1-D convolutions and independent dot products.
SmallVector<NestRecord, 4> findNests(LoopInfo &LI) {
  SmallVector<NestRecord, 4> out;
  for (Loop *L : LI.getLoopsInPreorder()) {
    if (!L->getSubLoops().empty())
      continue; // not an innermost loop
    SmallVector<Loop *, 4> chain;
    for (Loop *cur = L; cur != nullptr; cur = cur->getParentLoop())
      chain.push_back(cur);
    NestRecord rec;
    rec.outer = chain.back();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
      rec.levels.push_back(*it);
    out.push_back(std::move(rec));
  }
  return out;
}

// ----------------------------------------------------------------------------
// Phase 2: induction-variable recovery
// ----------------------------------------------------------------------------

bool recoverIVs(NestRecord &rec, ScalarEvolution &SE) {
  rec.ivs.clear();
  rec.ivNames.clear();
  for (Loop *L : rec.levels) {
    PHINode *iv = L->getInductionVariable(SE);
    if (!iv || !iv->getType()->isIntegerTy()) {
      rec.ivs.clear();
      rec.ivNames.clear();
      return false;
    }
    rec.ivs.push_back(iv);
    rec.ivNames.push_back(iv->hasName() ? iv->getName().str()
                                        : ("iv" + std::to_string(rec.ivs.size() - 1)));
  }
  return true;
}

// Count how many loops in the nest have shape-dependent bounds — i.e. their
// back-edge taken count's SCEV is not loop-invariant in some outer nest loop.
// This catches:
//   - syrk:  `for j in 0..i+1` → j-trip depends on i → triangular.
//   - trmm:  `for k in i..N`   → k-trip depends on i → triangular.
//   - cholesky-like double-triangular → 2 shape-dependent dims.
// Returns the count of shape-dependent loops. The synthesizer applies
// fill_factor = 0.5^count: 1 triangular dim → avg fill 0.5, 2 → 0.25, etc.
unsigned countTriangularLoops(const NestRecord &rec, ScalarEvolution &SE) {
  unsigned count = 0;
  for (Loop *L : rec.levels) {
    const SCEV *trip = SE.getBackedgeTakenCount(L);
    if (isa<SCEVCouldNotCompute>(trip))
      continue;
    if (isa<SCEVConstant>(trip))
      continue;
    for (Loop *OL : rec.levels) {
      if (OL == L)
        continue;
      if (!SE.isLoopInvariant(trip, OL)) {
        count++;
        break;
      }
    }
  }
  return count;
}

// ----------------------------------------------------------------------------
// Phase 3: per-memop SCEV affinity check + IV-dependency extraction
// ----------------------------------------------------------------------------

// Returns the set of nest loops whose AddRecs appear inside S.
// Returns nullopt if S contains:
//   * an AddRec for a loop NOT in the nest, or
//   * a SCEVUnknown that is not loop-invariant in every nest loop, or
//   * an unsupported SCEV node.
std::optional<DenseSet<const Loop *>>
classifyPtrSCEV(const SCEV *S, ScalarEvolution &SE,
                const SmallVectorImpl<Loop *> &nest) {
  DenseSet<const Loop *> deps;
  SmallPtrSet<const Loop *, 4> nestSet;
  for (Loop *L : nest)
    nestSet.insert(L);

  SmallVector<const SCEV *, 8> work = {S};
  while (!work.empty()) {
    const SCEV *cur = work.pop_back_val();
    if (isa<SCEVConstant>(cur))
      continue;
    if (isa<SCEVCouldNotCompute>(cur))
      return std::nullopt;
    if (auto *AR = dyn_cast<SCEVAddRecExpr>(cur)) {
      const Loop *L = AR->getLoop();
      if (!nestSet.count(L))
        return std::nullopt;
      deps.insert(L);
      work.push_back(AR->getStart());
      work.push_back(AR->getStepRecurrence(SE));
      continue;
    }
    if (auto *U = dyn_cast<SCEVUnknown>(cur)) {
      for (Loop *NL : nest) {
        if (!SE.isLoopInvariant(U, NL))
          return std::nullopt;
      }
      continue;
    }
    if (auto *Cast = dyn_cast<SCEVCastExpr>(cur)) {
      work.push_back(Cast->getOperand());
      continue;
    }
    if (auto *N = dyn_cast<SCEVNAryExpr>(cur)) {
      for (const SCEV *Op : N->operands())
        work.push_back(Op);
      continue;
    }
    if (auto *D = dyn_cast<SCEVUDivExpr>(cur)) {
      work.push_back(D->getLHS());
      work.push_back(D->getRHS());
      continue;
    }
    return std::nullopt; // unrecognized SCEV class
  }
  return deps;
}

// True iff BB belongs to the nest's chain. BB is on the chain iff there
// exists a deepest level L_d that contains it, AND for every ancestor level
// L_i (i <= d), BB is NOT inside any subloop of L_i other than L_{i+1}.
// This includes blocks that LICM has hoisted to outer levels (e.g. the
// C[i][j] load/store hoisted from the k-loop into the j-loop body), while
// still excluding sibling subloops (e.g. `for j: C[i][j] *= beta;` alongside
// the `for k: ... for j: MAC` nest in polybench gemm).
bool blockOnNestChain(BasicBlock *BB, const SmallVectorImpl<Loop *> &levels) {
  int deepest = -1;
  for (int i = 0; i < (int)levels.size(); ++i)
    if (levels[i]->contains(BB))
      deepest = i;
  if (deepest < 0)
    return false;
  for (int i = 0; i <= deepest; ++i) {
    Loop *L = levels[i];
    Loop *nextLevel =
        (i + 1 < (int)levels.size()) ? levels[i + 1] : nullptr;
    for (Loop *sub : L->getSubLoops()) {
      if (sub == nextLevel)
        continue;
      if (sub->contains(BB))
        return false;
    }
  }
  return true;
}

// Collect memops along the nest chain only. After -O1 LICM, the output store
// of a contraction is typically hoisted to a parent loop (above the
// reduction loop), so we walk the whole chain — but we exclude sibling
// subloops, which carry unrelated memops that would derail the affinity
// check.
// Returns false if any memop has a non-affine pointer SCEV (Phase 3 reject).
bool collectMemops(NestRecord &rec, ScalarEvolution &SE,
                   SmallVectorImpl<MemopInfo> &out,
                   std::string &rejectReason) {
  Loop *outer = rec.outer;
  for (BasicBlock *BB : outer->getBlocks()) {
    if (!blockOnNestChain(BB, rec.levels))
      continue;
    for (Instruction &I : *BB) {
      Value *ptrOp = nullptr;
      bool isStore = false;
      if (auto *LD = dyn_cast<LoadInst>(&I)) {
        ptrOp = LD->getPointerOperand();
      } else if (auto *ST = dyn_cast<StoreInst>(&I)) {
        ptrOp = ST->getPointerOperand();
        isStore = true;
      } else {
        continue;
      }
      const SCEV *ptrSCEV = SE.getSCEV(ptrOp);
      auto depsOpt = classifyPtrSCEV(ptrSCEV, SE, rec.levels);
      if (!depsOpt) {
        rejectReason = "non-affine memory access";
        return false;
      }
      MemopInfo mi;
      mi.inst = &I;
      mi.isStore = isStore;
      mi.basePtr = resolveBasePointer(ptrOp);
      mi.ptrSCEV = ptrSCEV;
      mi.deps = std::move(*depsOpt);
      out.push_back(std::move(mi));
    }
  }
  return true;
}

// ----------------------------------------------------------------------------
// Phase 4: SSA reduction recognition
//
// A loop is a reduction loop if its header has a PHI whose backedge value is
// `binop(phi, x)` for one of the supported reduction opcodes (add, fadd, mul,
// fmul, and/or/xor). We also accept the case where the backedge value is a
// chain `binop(... binop(phi, x) ...)`.
// ----------------------------------------------------------------------------

bool isReductionOpcode(unsigned op) {
  switch (op) {
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    return true;
  default:
    return false;
  }
}

bool walksBackToPhi(Value *V, PHINode *phi, unsigned reductionOp,
                    unsigned depthBudget) {
  if (V == phi)
    return true;
  if (depthBudget == 0)
    return false;
  auto *B = dyn_cast<BinaryOperator>(V);
  if (!B || B->getOpcode() != reductionOp)
    return false;
  return walksBackToPhi(B->getOperand(0), phi, reductionOp, depthBudget - 1) ||
         walksBackToPhi(B->getOperand(1), phi, reductionOp, depthBudget - 1);
}

// Returns true if V is a MAC compute: fadd(fmul, x), add(mul, x), or an
// llvm.fmuladd / llvm.fma intrinsic. If matched, sets *accumOut to the
// accumulator operand (the non-multiply input).
bool isMACCompute(Value *V, Value **accumOut = nullptr) {
  if (auto *II = dyn_cast<IntrinsicInst>(V)) {
    Intrinsic::ID id = II->getIntrinsicID();
    if (id == Intrinsic::fmuladd || id == Intrinsic::fma) {
      if (accumOut)
        *accumOut = II->getOperand(2);
      return true;
    }
    return false;
  }
  auto *Add = dyn_cast<BinaryOperator>(V);
  if (!Add || !isAddShape(Add->getOpcode()))
    return false;
  for (unsigned i = 0; i < 2; ++i) {
    auto *Mul = dyn_cast<BinaryOperator>(Add->getOperand(i));
    if (Mul && isMulShape(Mul->getOpcode()) && Mul->hasOneUse()) {
      if (accumOut)
        *accumOut = Add->getOperand(1 - i);
      return true;
    }
  }
  return false;
}

bool findReductionInLoop(Loop *L) {
  BasicBlock *header = L->getHeader();
  BasicBlock *latch = L->getLoopLatch();
  if (!latch)
    return false;
  for (PHINode &phi : header->phis()) {
    Value *bv = phi.getIncomingValueForBlock(latch);
    if (!bv)
      continue;
    Value *accum = nullptr;
    if (isMACCompute(bv, &accum) && accum == &phi)
      return true; // fmuladd/fma form or fadd(fmul, phi)
    auto *binop = dyn_cast<BinaryOperator>(bv);
    if (!binop || !isReductionOpcode(binop->getOpcode()))
      continue;
    if (walksBackToPhi(binop, &phi, binop->getOpcode(), /*depth=*/8))
      return true;
  }
  return false;
}

// ----------------------------------------------------------------------------
// Phase 5: MAC-dominated body check (datatype-agnostic).
// ----------------------------------------------------------------------------

bool isArithOpcode(unsigned op) {
  switch (op) {
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    return true;
  default:
    return false;
  }
}

// True if I or any transitive user (within depth hops) is a GEP.
// Used to distinguish address arithmetic from compute arithmetic: anything
// whose result flows into a pointer computation is address-arith.
bool feedsGEP(Instruction *I, unsigned depth = 4) {
  if (depth == 0)
    return false;
  for (User *u : I->users()) {
    if (isa<GetElementPtrInst>(u))
      return true;
    auto *ui = dyn_cast<Instruction>(u);
    if (ui && feedsGEP(ui, depth - 1))
      return true;
  }
  return false;
}

// An instruction is address arithmetic iff its result eventually feeds a GEP.
// Replaces the older "all-int-operands without a Load" heuristic, which was
// too strict for kernels that compute on int data (polybench gemm etc.) and
// rejected real compute as address-shape.
bool isAddressArith(Instruction *I) {
  return feedsGEP(I);
}

// Skip canonical IV updates of the form `add iv_phi, const` from arithmetic
// counts — they are not part of the body's compute.
bool isIVUpdate(Instruction *I) {
  if (I->getOpcode() != Instruction::Add)
    return false;
  bool constOp = isa<ConstantInt>(I->getOperand(0)) ||
                 isa<ConstantInt>(I->getOperand(1));
  bool phiOp = isa<PHINode>(I->getOperand(0)) || isa<PHINode>(I->getOperand(1));
  return constOp && phiOp;
}

// Sets *outMacs, *outStencil, *outTotal.
// stencil pair = add-shape op with BOTH operands load-derived (combining two
// loaded values; characteristic of stencil reduction trees like
// out = in[i-1][j] + in[i+1][j] + ...). Not double-counted with MACs.
void countBodyOps(Loop *inner, unsigned &outMacs, unsigned &outStencil,
                  unsigned &outTotal, unsigned &outMulOps,
                  unsigned &outAddOps) {
  outMacs = 0;
  outStencil = 0;
  outTotal = 0;
  outMulOps = 0;
  outAddOps = 0;
  for (BasicBlock *BB : inner->getBlocks()) {
    for (Instruction &I : *BB) {
      // fmuladd/fma intrinsic: counts as one MAC and one arith op.
      if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        Intrinsic::ID id = II->getIntrinsicID();
        if (id == Intrinsic::fmuladd || id == Intrinsic::fma) {
          outTotal++;
          outMacs++;
          outMulOps++;
          outAddOps++;
          continue;
        }
      }
      if (!isArithOpcode(I.getOpcode()))
        continue;
      if (isIVUpdate(&I))
        continue;
      if (isAddressArith(&I))
        continue;
      outTotal++;
      if (isMulShape(I.getOpcode()))
        outMulOps++;
      if (isAddShape(I.getOpcode()))
        outAddOps++;
      if (!isAddShape(I.getOpcode()))
        continue;
      // MAC pattern: add(mul, x) where the mul has a single use.
      // Datatype-agnostic. Address-arithmetic chains are already excluded
      // above by isAddressArith (result feeds a GEP).
      bool isMAC = false;
      for (unsigned i = 0; i < 2; ++i) {
        Value *op = I.getOperand(i);
        auto *bop = dyn_cast<BinaryOperator>(op);
        if (!bop || !isMulShape(bop->getOpcode()))
          continue;
        if (!bop->hasOneUse())
          continue;
        if (!inner->contains(bop->getParent()))
          continue;
        if (feedsGEP(bop))
          continue; // mul is part of a GEP index, not compute
        outMacs++;
        isMAC = true;
        break;
      }
      if (isMAC)
        continue;
      // Stencil-shape: an add combining two loaded values (no mul). Both
      // operands must be loads (or direct casts thereof).
      auto looksLoad = [](Value *v) {
        if (isa<LoadInst>(v))
          return true;
        if (auto *cast = dyn_cast<CastInst>(v))
          return isa<LoadInst>(cast->getOperand(0));
        return false;
      };
      if (looksLoad(I.getOperand(0)) && looksLoad(I.getOperand(1)))
        outStencil++;
    }
  }
}

bool collectIntegerWeightedTerms(Value *V, int64_t scale,
                                 SmallVectorImpl<WeightedTerm> &terms,
                                 unsigned depthBudget) {
  if (depthBudget == 0)
    return false;
  if (auto *cast = dyn_cast<CastInst>(V))
    return collectIntegerWeightedTerms(cast->getOperand(0), scale, terms,
                                       depthBudget - 1);
  if (auto *load = dyn_cast<LoadInst>(V)) {
    terms.push_back({load, scale});
    return true;
  }
  auto *bin = dyn_cast<BinaryOperator>(V);
  if (!bin)
    return false;
  if (bin->getOpcode() == Instruction::Add) {
    return collectIntegerWeightedTerms(bin->getOperand(0), scale, terms,
                                       depthBudget - 1) &&
           collectIntegerWeightedTerms(bin->getOperand(1), scale, terms,
                                       depthBudget - 1);
  }
  if (bin->getOpcode() == Instruction::Sub) {
    if (scale == std::numeric_limits<int64_t>::min())
      return false;
    return collectIntegerWeightedTerms(bin->getOperand(0), scale, terms,
                                       depthBudget - 1) &&
           collectIntegerWeightedTerms(bin->getOperand(1), -scale, terms,
                                       depthBudget - 1);
  }
  if (bin->getOpcode() == Instruction::Mul) {
    for (unsigned constantOperand = 0; constantOperand < 2;
         ++constantOperand) {
      auto *constant = dyn_cast<ConstantInt>(bin->getOperand(constantOperand));
      if (!constant || constant->getBitWidth() > 64)
        continue;
      int64_t product = 0;
      if (__builtin_mul_overflow(scale, constant->getSExtValue(), &product))
        return false;
      return collectIntegerWeightedTerms(
          bin->getOperand(1 - constantOperand), product, terms,
          depthBudget - 1);
    }
    return false;
  }
  if (bin->getOpcode() == Instruction::Shl) {
    auto *shift = dyn_cast<ConstantInt>(bin->getOperand(1));
    if (!shift || shift->getValue().uge(63))
      return false;
    int64_t factor = int64_t{1} << shift->getZExtValue();
    int64_t product = 0;
    if (__builtin_mul_overflow(scale, factor, &product))
      return false;
    return collectIntegerWeightedTerms(bin->getOperand(0), product, terms,
                                       depthBudget - 1);
  }
  return false;
}

const MemopInfo *findMemopForInstruction(
    const SmallVectorImpl<MemopInfo> &memops, const Instruction *inst) {
  for (const MemopInfo &memop : memops)
    if (memop.inst == inst)
      return &memop;
  return nullptr;
}

// ----------------------------------------------------------------------------
// Phase 7: impure-call check
// ----------------------------------------------------------------------------

bool hasImpureCalls(Loop *inner) {
  for (BasicBlock *BB : inner->getBlocks()) {
    for (Instruction &I : *BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      // Morpher marker call is a no-op annotation, not real compute.
      Function *F = CB->getCalledFunction();
      if (!F)
        F = dyn_cast<Function>(CB->getCalledOperand()->stripPointerCasts());
      if (F)
        if (F->getName() == "please_map_me")
          continue;
      if (CB->onlyReadsMemory() || CB->doesNotAccessMemory())
        continue;
      if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
        switch (II->getIntrinsicID()) {
        case Intrinsic::fmuladd:
        case Intrinsic::fma:
        case Intrinsic::fabs:
        case Intrinsic::sqrt:
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
        case Intrinsic::assume:
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        case Intrinsic::dbg_label:
          continue;
        default:
          break;
        }
      }
      return true;
    }
  }
  return false;
}

// ----------------------------------------------------------------------------
// Phase 8: alias-safety check.
//
// Conservative: if all distinct base pointers among memops are either
//   - function arguments with `noalias`, or
//   - distinct allocas / globals,
// pass. Else, demote.
// ----------------------------------------------------------------------------

bool baseIsSafe(Value *base) {
  if (auto *arg = dyn_cast<Argument>(base))
    return arg->hasNoAliasAttr();
  if (isa<AllocaInst>(base))
    return true;
  if (isa<GlobalVariable>(base))
    return true;
  return false;
}

bool hasUnsafeAliasing(SmallVectorImpl<MemopInfo> &memops, AAResults &AA) {
  SmallPtrSet<Value *, 4> distinct;
  for (auto &m : memops)
    distinct.insert(m.basePtr);
  SmallVector<Value *, 4> bases(distinct.begin(), distinct.end());

  // If every base is independently safe (noalias arg / alloca / global),
  // they don't alias each other.
  bool allSafe = true;
  for (Value *b : bases)
    if (!baseIsSafe(b)) {
      allSafe = false;
      break;
    }
  if (allSafe)
    return false;

  // Otherwise: rely on AA. If any pair MayAlias, flag.
  for (unsigned i = 0; i < bases.size(); ++i)
    for (unsigned j = i + 1; j < bases.size(); ++j) {
      AliasResult r = AA.alias(MemoryLocation::getBeforeOrAfter(bases[i]),
                               MemoryLocation::getBeforeOrAfter(bases[j]));
      if (r != AliasResult::NoAlias)
        return true;
    }
  return false;
}

// ----------------------------------------------------------------------------
// Phase 6 (cross-check) + dataflow hints
// ----------------------------------------------------------------------------

// Collect all stores (Phase 3 relaxation: was unique-store-only).
SmallVector<const MemopInfo *, 4>
collectStores(const SmallVectorImpl<MemopInfo> &memops) {
  SmallVector<const MemopInfo *, 4> stores;
  SmallPtrSet<Value *, 4> seen;
  for (auto &m : memops) {
    if (!m.isStore)
      continue;
    if (seen.insert(m.basePtr).second)
      stores.push_back(&m);
  }
  return stores;
}

// dataflow_hints set based on contraction shape.
SmallVector<std::string, 3>
pickHints(NestRecord &rec, SmallVectorImpl<MemopInfo> &memops,
          const MemopInfo *storeOp, const DenseSet<const Loop *> &outputDeps,
          const DenseSet<const Loop *> &reductionLoops) {
  SmallVector<std::string, 3> hints;
  // OS is legal if the output depends on every "parallel" IV and on none of
  // the reduction IVs.
  bool osLegal = true;
  for (const Loop *L : reductionLoops)
    if (outputDeps.count(L)) { osLegal = false; break; }
  if (osLegal) hints.push_back("output_stationary");

  // WS/IS: one input depends on (1 parallel ∪ all reduction) while another
  // input depends on (the other parallel ∪ all reduction). We enumerate the
  // parallel IVs (= outputDeps), require exactly two, and check the inputs.
  SmallVector<const Loop *, 2> parallels(outputDeps.begin(), outputDeps.end());
  if (parallels.size() == 2 && !reductionLoops.empty()) {
    // For each parallel p, check if any input memop has deps == {p} ∪ reduction.
    auto matchInputShape = [&](const Loop *p) -> bool {
      for (auto &m : memops) {
        if (m.isStore) continue;
        if (storeOp && m.basePtr == storeOp->basePtr) continue;
        if (m.deps.count(p) == 0) continue;
        // must NOT contain the other parallel
        const Loop *other = (parallels[0] == p) ? parallels[1] : parallels[0];
        if (m.deps.count(other)) continue;
        // must contain at least one reduction loop
        bool hasRed = false;
        for (const Loop *r : reductionLoops)
          if (m.deps.count(r)) { hasRed = true; break; }
        if (hasRed) return true;
      }
      return false;
    };
    if (matchInputShape(parallels[0])) hints.push_back("weight_stationary");
    if (matchInputShape(parallels[1])) hints.push_back("input_stationary");
  }
  return hints;
}


} // namespace morpher_systolic
