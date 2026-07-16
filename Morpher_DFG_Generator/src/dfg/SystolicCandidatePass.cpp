// SystolicCandidatePass detects affine tensor-contraction loop nests and emits
// a versioned transform recipe. The companion pipeline consumes IR-native
// recipe metadata and performs a restricted systolic unroll-and-jam transform.
// See REFERENCE.md for the intentionally narrow legality contract.

#include "morpherdfggen/dfg/SystolicCandidatePass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopFuse.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollAndJamPass.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace {

static cl::opt<std::string> MorpherSystolicReport(
    "morpher-systolic-report", cl::init(""),
    cl::desc("Write SystolicCandidatePass JSON report to this file (default: stderr)"));

static cl::opt<std::string> MorpherSystolicTransformFunction(
    "morpher-systolic-fn", cl::init(""),
    cl::desc("Function containing the systolic candidate to transform"));

static cl::opt<unsigned> MorpherSystolicCandidateIndex(
    "morpher-systolic-candidate-index", cl::init(0),
    cl::desc("Zero-based systolic candidate index within the target function"));

static cl::opt<unsigned> MorpherSystolicFactor0(
    "morpher-systolic-factor0", cl::init(0),
    cl::desc("Exact unroll-and-jam factor for the first parallel dimension"));

static cl::opt<unsigned> MorpherSystolicFactor1(
    "morpher-systolic-factor1", cl::init(0),
    cl::desc("Exact unroll-and-jam factor for the second parallel dimension"));

static cl::opt<bool> MorpherSystolicFPNondeterminism(
    "morpher-systolic-fp-nondeterminism", cl::init(true),
    cl::desc("Permit floating-point reassociation during systolic output canonicalization"));

constexpr StringLiteral SystolicRecipeMetadata =
    "morpher.systolic.transform.recipe.v1";
constexpr StringLiteral SystolicReductionMetadata =
    "morpher.systolic.rolled_reduction.v1";
constexpr StringLiteral SystolicMemStreamMetadata =
    "morpher.systolic.mem_stream";
constexpr StringLiteral SystolicMemStreamIndexMetadata =
    "morpher.systolic.mem_stream_index";
constexpr StringLiteral SystolicMacCoordMetadata =
    "morpher.systolic.mac_coord";
constexpr StringLiteral SystolicMemStreamBindingsAttr =
    "morpher-systolic-mem-stream-bindings";
constexpr StringLiteral SystolicStrippedMarkerAttr =
    "morpher-systolic-stripped-map-marker";

// ----------------------------------------------------------------------------
// Data structures
// ----------------------------------------------------------------------------

struct NestRecord {
  Loop *outer = nullptr;
  SmallVector<Loop *, 4> levels;       // outer-to-inner
  SmallVector<PHINode *, 4> ivs;       // canonical IV per level
  SmallVector<std::string, 4> ivNames; // pretty names for report
};

struct MemopInfo {
  Instruction *inst = nullptr;
  bool isStore = false;
  Value *basePtr = nullptr;
  const SCEV *ptrSCEV = nullptr;
  DenseSet<const Loop *> deps; // nest loops this memop's pointer SCEV depends on
};

struct WeightedTerm {
  const LoadInst *load = nullptr;
  int64_t coefficient = 0;
};

struct TransformDimension {
  unsigned level = 0;
  uint64_t tripCount = 0;
  SmallVector<uint64_t, 8> exactDivisors;
};

struct TransformRecipe {
  static constexpr unsigned SchemaVersion = 1;

  bool supported = false;
  std::string reason;
  SmallVector<TransformDimension, 2> parallelDimensions;
  unsigned reductionLevel = 0;
  uint64_t reductionTripCount = 0;
};

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

Value *resolveBasePointer(Value *PtrOp) {
  Value *cur = PtrOp;
  while (true) {
    if (auto *GEP = dyn_cast<GEPOperator>(cur)) {
      cur = GEP->getPointerOperand();
      continue;
    }
    if (auto *BC = dyn_cast<BitCastOperator>(cur)) {
      cur = BC->getOperand(0);
      continue;
    }
    break;
  }
  return cur;
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
// Opcode shape predicates (used by Phase 4 and Phase 5)
// ----------------------------------------------------------------------------

bool isAddShape(unsigned op) { return op == Instruction::Add || op == Instruction::FAdd; }
bool isMulShape(unsigned op) { return op == Instruction::Mul || op == Instruction::FMul; }

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
                                 unsigned depthBudget = 32) {
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

// ----------------------------------------------------------------------------
// Phase 9b: LoopID metadata attachment
// ----------------------------------------------------------------------------

Metadata *unsignedMetadata(LLVMContext &Ctx, uint64_t value) {
  return ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt64Ty(Ctx), value));
}

void attachLoopMetadata(Loop *L, ArrayRef<std::string> hints,
                        const TransformRecipe &recipe,
                        unsigned candidateIndex) {
  LLVMContext &Ctx = L->getHeader()->getContext();
  SmallVector<Metadata *, 8> ops;
  TempMDTuple selfRef = MDNode::getTemporary(Ctx, std::nullopt);
  ops.push_back(selfRef.get()); // placeholder for self-reference

  SmallVector<Metadata *, 4> propOps;
  propOps.push_back(MDString::get(Ctx, "systolic.candidate"));
  for (const std::string &h : hints)
    propOps.push_back(MDString::get(Ctx, h));
  ops.push_back(MDNode::get(Ctx, propOps));

  if (recipe.supported) {
    SmallVector<Metadata *, 9> recipeOps;
    recipeOps.push_back(MDString::get(Ctx, SystolicRecipeMetadata));
    recipeOps.push_back(unsignedMetadata(Ctx, TransformRecipe::SchemaVersion));
    recipeOps.push_back(unsignedMetadata(Ctx, candidateIndex));
    for (const TransformDimension &dimension : recipe.parallelDimensions) {
      recipeOps.push_back(unsignedMetadata(Ctx, dimension.level));
      recipeOps.push_back(unsignedMetadata(Ctx, dimension.tripCount));
    }
    recipeOps.push_back(unsignedMetadata(Ctx, recipe.reductionLevel));
    recipeOps.push_back(unsignedMetadata(Ctx, recipe.reductionTripCount));
    ops.push_back(MDNode::get(Ctx, recipeOps));
  }

  // Preserve existing LoopID operands (skip its self-ref at index 0).
  if (MDNode *existing = L->getLoopID())
    for (unsigned i = 1; i < existing->getNumOperands(); ++i)
      ops.push_back(existing->getOperand(i));

  MDNode *newID = MDNode::getDistinct(Ctx, ops);
  newID->replaceOperandWith(0, newID); // resolve self-reference
  L->setLoopID(newID);
}

// Record only the two contraction input streams.  The first transformed
// parallel dimension is the PE-row dimension, so an input indexed by it and
// the reduction dimension enters from the left.  The second is the PE-column
// dimension and enters from the bottom.  The binding lives in a temporary
// function string attribute during LLVM cleanup because GVN intentionally
// drops unknown instruction metadata.  A final pass restores per-load
// metadata after sharing is done.
// Output seed loads share a base with a store and deliberately remain
// unrestricted.
void attachSystolicMemoryStreams(
    NestRecord &rec, const SmallVectorImpl<MemopInfo> &memops,
    ArrayRef<const MemopInfo *> stores, const TransformRecipe &recipe) {
  if (!recipe.supported || recipe.parallelDimensions.size() != 2 ||
      recipe.reductionLevel >= rec.levels.size())
    return;
  const unsigned rowLevel = recipe.parallelDimensions[0].level;
  const unsigned columnLevel = recipe.parallelDimensions[1].level;
  if (rowLevel >= rec.levels.size() || columnLevel >= rec.levels.size())
    return;

  Loop *rowLoop = rec.levels[rowLevel];
  Loop *columnLoop = rec.levels[columnLevel];
  Loop *reductionLoop = rec.levels[recipe.reductionLevel];
  SmallPtrSet<Value *, 4> outputBases;
  for (const MemopInfo *store : stores)
    outputBases.insert(store->basePtr);

  auto baseKey = [](Value *base) -> std::string {
    if (auto *argument = dyn_cast<Argument>(base))
      return "a" + std::to_string(argument->getArgNo());
    if (auto *global = dyn_cast<GlobalValue>(base))
      return "g" + std::to_string(GlobalValue::getGUID(global->getName()));
    if (auto *alloca = dyn_cast<AllocaInst>(base)) {
      if (alloca->getName().empty())
        alloca->setName("morpher.systolic.base");
      return "l" + std::to_string(GlobalValue::getGUID(alloca->getName()));
    }
    return "";
  };
  std::string bindings;
  SmallPtrSet<Value *, 4> boundBases;
  for (const MemopInfo &memop : memops) {
    if (memop.isStore || outputBases.count(memop.basePtr) ||
        !memop.deps.count(reductionLoop))
      continue;
    StringRef role;
    if (memop.deps.count(rowLoop) && !memop.deps.count(columnLoop))
      role = "row";
    else if (memop.deps.count(columnLoop) && !memop.deps.count(rowLoop))
      role = "column";
    else
      continue;
    if (!boundBases.insert(memop.basePtr).second)
      continue;
    std::string key = baseKey(memop.basePtr);
    if (key.empty())
      continue;
    if (!bindings.empty())
      bindings += ",";
    bindings += role.str() + ":" + key;
  }
  if (!bindings.empty())
    rec.outer->getHeader()->getParent()->addFnAttr(
        SystolicMemStreamBindingsAttr, bindings);
}

// ----------------------------------------------------------------------------
// Phase 9a: JSON report
// ----------------------------------------------------------------------------

void writeQuoted(raw_ostream &OS, StringRef S) {
  OS << '"';
  for (char c : S) {
    if (c == '"' || c == '\\')
      OS << '\\';
    OS << c;
  }
  OS << '"';
}

std::string valueName(Value *V, StringRef fallback) {
  if (V && V->hasName())
    return V->getName().str();
  return fallback.str();
}

std::string opcodeSummary(const Instruction *I) {
  if (!I)
    return "unknown";
  if (auto *LD = dyn_cast<LoadInst>(I))
    return (Twine("load.") + Twine(LD->getType()->getScalarSizeInBits())).str();
  if (auto *ST = dyn_cast<StoreInst>(I))
    return (Twine("store.") +
            Twine(ST->getValueOperand()->getType()->getScalarSizeInBits())).str();
  return I->getOpcodeName();
}

std::optional<uint64_t> constantTripCount(Loop *L, ScalarEvolution &SE) {
  const SCEV *backedge = SE.getBackedgeTakenCount(L);
  if (auto *C = dyn_cast<SCEVConstant>(backedge)) {
    const APInt &v = C->getAPInt();
    if (!v.isNegative())
      return v.getZExtValue() + 1;
  }
  unsigned small = SE.getSmallConstantTripCount(L);
  if (small > 0)
    return small;
  return std::nullopt;
}

SmallVector<uint64_t, 8> exactUnrollDivisors(uint64_t tripCount) {
  SmallVector<uint64_t, 8> out;
  for (uint64_t factor = 2; factor <= tripCount / factor; ++factor) {
    if (tripCount % factor != 0)
      continue;
    out.push_back(factor);
    uint64_t pairedFactor = tripCount / factor;
    if (pairedFactor != factor)
      out.push_back(pairedFactor);
  }
  if (tripCount > 1)
    out.push_back(tripCount);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool isPerfectThreeLevelChain(const NestRecord &rec) {
  if (rec.levels.size() != 3)
    return false;
  for (unsigned level = 0; level + 1 < rec.levels.size(); ++level) {
    ArrayRef<Loop *> subLoops = rec.levels[level]->getSubLoops();
    if (subLoops.size() != 1 || subLoops.front() != rec.levels[level + 1])
      return false;
  }
  return rec.levels.back()->getSubLoops().empty();
}

TransformRecipe buildTransformRecipe(
    const NestRecord &rec, LoopInfo &LI, ScalarEvolution &SE, StringRef verdict,
    StringRef family, StringRef rejectReason,
    const DenseSet<const Loop *> &reductionLoops,
    const DenseSet<const Loop *> &outputDeps,
    ArrayRef<const MemopInfo *> stores) {
  TransformRecipe recipe;
  auto unsupported = [&](Twine reason) {
    recipe.reason = reason.str();
    return recipe;
  };

  if (verdict == "no" || !rejectReason.empty())
    return unsupported(rejectReason.empty() ? "candidate rejected by detector"
                                            : rejectReason);
  if (family != "gemm_like_2d_reduction")
    return unsupported("lowering family is not gemm_like_2d_reduction");
  if (!isPerfectThreeLevelChain(rec))
    return unsupported("v1 requires a canonical perfect three-level nest");
  if (LI.getLoopsInPreorder().size() != 3)
    return unsupported("v1 requires the candidate to be the function's only loop nest");
  if (stores.size() != 1)
    return unsupported("v1 requires exactly one output base");
  if (reductionLoops.size() != 1)
    return unsupported("v1 requires exactly one reduction loop");

  const Loop *reduction = *reductionLoops.begin();
  if (rec.levels.back() != reduction)
    return unsupported("v1 requires the reduction loop to be innermost");
  if (outputDeps.size() != 2 || !outputDeps.count(rec.levels[0]) ||
      !outputDeps.count(rec.levels[1]))
    return unsupported("v1 requires the two outer loops to be parallel output dimensions");

  for (unsigned level = 0; level < rec.levels.size(); ++level) {
    auto tripCount = constantTripCount(rec.levels[level], SE);
    if (!tripCount || *tripCount == 0)
      return unsupported("v1 requires constant positive trip counts");
    if (level < 2) {
      TransformDimension dimension;
      dimension.level = level;
      dimension.tripCount = *tripCount;
      dimension.exactDivisors = exactUnrollDivisors(*tripCount);
      if (dimension.exactDivisors.empty())
        return unsupported("parallel trip count has no non-trivial exact divisor");
      recipe.parallelDimensions.push_back(std::move(dimension));
    } else {
      recipe.reductionLevel = level;
      recipe.reductionTripCount = *tripCount;
    }
  }

  recipe.supported = true;
  return recipe;
}

StringRef loweringFamilyFor(Function &F, const NestRecord &rec,
                            const DenseSet<const Loop *> &reductionLoops,
                            unsigned macs, unsigned stencilPairs,
                            bool exactWeightedSum) {
  (void)F;
  unsigned parallel = 0;
  for (Loop *L : rec.levels)
    if (!reductionLoops.count(L))
      parallel++;
  if (parallel == 1 && reductionLoops.size() == 1 && macs > 0)
    return "dot_1d_reduction";
  if (parallel >= 1 && reductionLoops.empty() &&
      (macs > 0 || stencilPairs > 0 || exactWeightedSum))
    return parallel == 2 ? "stencil2d_weighted_sum" : "fixed_weighted_sum";
  if (parallel == 2 && reductionLoops.size() == 1 && macs > 0)
    return "gemm_like_2d_reduction";
  return "unsupported";
}

bool sourceEquivalentCandidateFor(StringRef family, StringRef verdict,
                                  StringRef rejectReason) {
  return verdict != "no" && rejectReason.empty() && family != "unsupported";
}

void writeJSON(raw_ostream &OS, Function &F, NestRecord &rec,
               ScalarEvolution &SE, unsigned candidateIndex,
               StringRef verdict, ArrayRef<std::string> hints,
               const SmallVectorImpl<MemopInfo> &memops,
               const DenseSet<const Loop *> &reductionLoops,
               const DenseSet<const Loop *> &outputDeps, unsigned macs,
               unsigned stencilPairs, unsigned totalArith, unsigned mulOps,
               unsigned addOps,
               ArrayRef<const MemopInfo *> stores,
               unsigned triangularLoops, double kernelShapeFactor,
               StringRef rejectReason, const TransformRecipe &recipe) {
  OS << "{";
  OS << "\"function\":";
  writeQuoted(OS, F.getName());
  OS << ",\"candidate_index\":" << candidateIndex;
  OS << ",\"loop_header\":";
  writeQuoted(OS, rec.outer->getHeader()->getName());
  OS << ",\"verdict\":";
  writeQuoted(OS, verdict);
  OS << ",\"depth\":" << rec.levels.size();
  OS << ",\"trip_counts\":[";
  for (unsigned i = 0; i < rec.levels.size(); ++i) {
    if (i) OS << ",";
    OS << "{\"level\":" << i;
    OS << ",\"iv\":";
    std::string ivName = i < rec.ivNames.size()
                             ? rec.ivNames[i]
                             : ("iv" + std::to_string(i));
    writeQuoted(OS, ivName);
    OS << ",\"trip_count\":";
    auto tc = constantTripCount(rec.levels[i], SE);
    if (tc)
      OS << *tc;
    else
      OS << "null";
    OS << "}";
  }
  OS << "]";
  OS << ",\"loop_classes\":[";
  for (unsigned i = 0; i < rec.levels.size(); ++i) {
    if (i) OS << ",";
    OS << "{\"level\":" << i;
    OS << ",\"kind\":";
    StringRef kind = "PARALLEL";
    if (reductionLoops.count(rec.levels[i])) kind = "REDUCTION";
    writeQuoted(OS, kind);
    OS << ",\"iv\":";
    std::string ivName = i < rec.ivNames.size()
                             ? rec.ivNames[i]
                             : ("iv" + std::to_string(i));
    writeQuoted(OS, ivName);
    OS << "}";
  }
  OS << "]";
  OS << ",\"mac_pairs\":" << macs;
  OS << ",\"stencil_pairs\":" << stencilPairs;
  OS << ",\"total_arith\":" << totalArith;
  OS << ",\"arithmetic_summary\":{";
  OS << "\"mac_pairs\":" << macs;
  OS << ",\"stencil_pairs\":" << stencilPairs;
  OS << ",\"total_arith\":" << totalArith;
  OS << ",\"mul_ops\":" << mulOps;
  OS << ",\"add_ops\":" << addOps;
  // Constant multiplications may be strength-reduced into shifts/adds before
  // this pass.  For a fixed weighted sum, add-tree arity is therefore often
  // stronger evidence than the number of surviving MUL instructions.
  unsigned weightedTerms = std::max(mulOps, addOps > 0 ? addOps + 1 : 0);
  OS << ",\"weighted_terms\":" << weightedTerms;
  SmallVector<WeightedTerm, 8> weightedSumTerms;
  bool coefficientsKnown = false;
  if (reductionLoops.empty() && stores.size() == 1) {
    if (auto *store = dyn_cast<StoreInst>(stores[0]->inst)) {
      coefficientsKnown = collectIntegerWeightedTerms(
          store->getValueOperand(), 1, weightedSumTerms);
      if (!coefficientsKnown)
        weightedSumTerms.clear();
    }
  }
  OS << ",\"coefficients_known\":"
     << (coefficientsKnown ? "true" : "false");
  OS << ",\"weighted_sum_terms\":[";
  for (unsigned i = 0; i < weightedSumTerms.size(); ++i) {
    if (i)
      OS << ",";
    const WeightedTerm &term = weightedSumTerms[i];
    const MemopInfo *memop = findMemopForInstruction(memops, term.load);
    OS << "{\"coefficient\":" << term.coefficient;
    OS << ",\"base\":";
    writeQuoted(OS, memop ? valueName(memop->basePtr, "anon") : "unknown");
    OS << ",\"pointer_scev\":";
    if (memop && memop->ptrSCEV) {
      std::string scevText;
      raw_string_ostream scevOS(scevText);
      memop->ptrSCEV->print(scevOS);
      scevOS.flush();
      writeQuoted(OS, scevText);
    } else {
      OS << "null";
    }
    OS << "}";
  }
  OS << "]";
  OS << ",\"supported_ops\":[";
  bool wroteOp = false;
  if (macs > 0) {
    writeQuoted(OS, "mul_add");
    wroteOp = true;
  }
  if (stencilPairs > 0) {
    if (wroteOp) OS << ",";
    writeQuoted(OS, "add_tree");
  }
  OS << "]}";
  OS << ",\"memops\":" << memops.size();
  OS << ",\"affine_accesses\":[";
  for (unsigned i = 0; i < memops.size(); ++i) {
    if (i) OS << ",";
    const MemopInfo &m = memops[i];
    OS << "{\"kind\":";
    writeQuoted(OS, m.isStore ? "store" : "load");
    OS << ",\"base\":";
    writeQuoted(OS, valueName(m.basePtr, (Twine("anon") + Twine(i)).str()));
    OS << ",\"opcode\":";
    writeQuoted(OS, opcodeSummary(m.inst));
    OS << ",\"pointer_scev\":";
    if (m.ptrSCEV) {
      std::string scevText;
      raw_string_ostream scevOS(scevText);
      m.ptrSCEV->print(scevOS);
      scevOS.flush();
      writeQuoted(OS, scevText);
    } else {
      OS << "null";
    }
    OS << ",\"loop_coefficients\":{";
    bool first = true;
    for (unsigned l = 0; l < rec.levels.size(); ++l) {
      if (!m.deps.count(rec.levels[l]))
        continue;
      if (!first) OS << ",";
      first = false;
      std::string ivName = l < rec.ivNames.size()
                               ? rec.ivNames[l]
                               : ("iv" + std::to_string(l));
      writeQuoted(OS, ivName);
      OS << ":1";
    }
    OS << "}}";
  }
  OS << "]";
  OS << ",\"triangular_loops\":" << triangularLoops;
  // Emit as fixed-point float so downstream JSON parsers see a number.
  char buf[32];
  snprintf(buf, sizeof(buf), "%.4f", kernelShapeFactor);
  OS << ",\"kernel_shape_factor\":" << buf;
  OS << ",\"output_bases\":[";
  for (unsigned i = 0; i < stores.size(); ++i) {
    if (i) OS << ",";
    std::string n;
    if (stores[i]->basePtr->hasName())
      n = stores[i]->basePtr->getName().str();
    else
      n = "anon" + std::to_string(i);
    writeQuoted(OS, n);
  }
  OS << "]";
  OS << ",\"dataflow_hints\":[";
  for (unsigned i = 0; i < hints.size(); ++i) {
    if (i) OS << ",";
    writeQuoted(OS, hints[i]);
  }
  OS << "]";
  StringRef family = loweringFamilyFor(F, rec, reductionLoops, macs,
                                       stencilPairs, coefficientsKnown);
  OS << ",\"lowering_family\":";
  writeQuoted(OS, family);
  OS << ",\"transform_recipe\":{";
  OS << "\"schema_version\":" << TransformRecipe::SchemaVersion;
  OS << ",\"candidate_id\":";
  writeQuoted(OS, (Twine(F.getName()) + "#" + Twine(candidateIndex)).str());
  OS << ",\"transform_supported\":"
     << (recipe.supported ? "true" : "false");
  OS << ",\"unsupported_reason\":";
  if (recipe.supported)
    OS << "null";
  else
    writeQuoted(OS, recipe.reason);
  OS << ",\"parallel_dimensions\":[";
  for (unsigned dimensionIndex = 0;
       dimensionIndex < recipe.parallelDimensions.size(); ++dimensionIndex) {
    if (dimensionIndex)
      OS << ",";
    const TransformDimension &dimension =
        recipe.parallelDimensions[dimensionIndex];
    OS << "{\"level\":" << dimension.level;
    OS << ",\"trip_count\":" << dimension.tripCount;
    OS << ",\"requires_exact_divisor\":true";
    OS << ",\"legal_factors\":[";
    for (unsigned factorIndex = 0;
         factorIndex < dimension.exactDivisors.size(); ++factorIndex) {
      if (factorIndex)
        OS << ",";
      OS << dimension.exactDivisors[factorIndex];
    }
    OS << "]}";
  }
  OS << "]";
  OS << ",\"rolled_reduction\":";
  if (recipe.supported) {
    OS << "{\"level\":" << recipe.reductionLevel;
    OS << ",\"trip_count\":" << recipe.reductionTripCount << "}";
  } else {
    OS << "null";
  }
  OS << "}";
  OS << ",\"source_equivalent_candidate\":"
     << (sourceEquivalentCandidateFor(family, verdict, rejectReason) ? "true" : "false");
  OS << ",\"systolic_reject_reason\":";
  if (sourceEquivalentCandidateFor(family, verdict, rejectReason))
    OS << "null";
  else if (!rejectReason.empty())
    writeQuoted(OS, rejectReason);
  else
    writeQuoted(OS, "unsupported source-equivalent lowering family");
  if (verdict != "no") {
    OS << ",\"recommended_dfg_pipeline\":{\"externalize_access\":\"agi_iter\"}";
  }
  OS << ",\"reject_reason\":";
  if (rejectReason.empty())
    OS << "null";
  else
    writeQuoted(OS, rejectReason);
  OS << "}\n";
}

// ----------------------------------------------------------------------------
// Output streaming (file or stderr). Serialized across threads.
// ----------------------------------------------------------------------------

std::mutex &reportMutex() {
  static std::mutex M;
  return M;
}

void emitReport(const std::string &json) {
  std::lock_guard<std::mutex> g(reportMutex());
  if (MorpherSystolicReport.empty()) {
    errs() << json;
    return;
  }
  std::ofstream out(MorpherSystolicReport.getValue(), std::ios::app);
  out << json;
}

// ----------------------------------------------------------------------------
// Main pass driver
// ----------------------------------------------------------------------------

struct SystolicCandidatePass : public PassInfoMixin<SystolicCandidatePass> {
  explicit SystolicCandidatePass(bool emitJSONReport = true)
      : emitJSONReport(emitJSONReport) {}

  bool emitJSONReport;

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
    ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    (void)AM.getResult<DominatorTreeAnalysis>(F);
    DependenceInfo &DI = AM.getResult<DependenceAnalysis>(F);
    AAResults &AA = AM.getResult<AAManager>(F);

    auto nests = findNests(LI);
    for (unsigned candidateIndex = 0; candidateIndex < nests.size();
         ++candidateIndex) {
      NestRecord &rec = nests[candidateIndex];
      std::string rejectReason;
      std::string verdict = "yes";
      SmallVector<std::string, 3> hints;
      SmallVector<MemopInfo, 8> memops;
      DenseSet<const Loop *> reductionLoops;
      DenseSet<const Loop *> outputDeps;
      unsigned macs = 0;
      unsigned stencilPairs = 0;
      unsigned totalArith = 0;
      unsigned mulOps = 0;
      unsigned addOps = 0;
      SmallVector<const MemopInfo *, 4> stores;
      unsigned triangularLoops = 0;
      double kernelShapeFactor = 1.0;

      auto reportThis = [&]() {
        StringRef transformFamily = loweringFamilyFor(
            F, rec, reductionLoops, macs, stencilPairs,
            /*exactWeightedSum=*/false);
        TransformRecipe recipe = buildTransformRecipe(
            rec, LI, SE, verdict, transformFamily, rejectReason, reductionLoops,
            outputDeps, stores);
        std::string buf;
        raw_string_ostream OS(buf);
        writeJSON(OS, F, rec, SE, candidateIndex, verdict, hints, memops, reductionLoops,
                  outputDeps, macs, stencilPairs, totalArith, mulOps, addOps,
                  stores,
                  triangularLoops, kernelShapeFactor, rejectReason, recipe);
        OS.flush();
        if (emitJSONReport)
          emitReport(buf);
        if (verdict != "no") {
          // The temporary binding attribute is an implementation detail of
          // the consuming transform and must not leak from report-only runs.
          if (!emitJSONReport)
            attachSystolicMemoryStreams(rec, memops, stores, recipe);
          attachLoopMetadata(rec.outer, hints, recipe, candidateIndex);
        }
      };

      if (!recoverIVs(rec, SE)) {
        verdict = "no";
        rejectReason = "non-canonical induction variable";
        reportThis();
        continue;
      }

      // Compute shape factor as soon as IVs are recovered so it's emitted
      // even when downstream checks fail. 0.5^triangular_count gives the
      // average fill of the iteration polytope: 1 triangular dim ⇒ 0.5,
      // 2 triangular dims ⇒ 0.25 (e.g. cholesky-shape), etc.
      triangularLoops = countTriangularLoops(rec, SE);
      kernelShapeFactor = 1.0;
      for (unsigned i = 0; i < triangularLoops; ++i)
        kernelShapeFactor *= 0.5;

      if (!collectMemops(rec, SE, memops, rejectReason)) {
        verdict = "no";
        reportThis();
        continue;
      }

      if (memops.empty()) {
        verdict = "no";
        rejectReason = "no memory operations in nest";
        reportThis();
        continue;
      }

      // Phase 7 (early): impure-call check.
      if (hasImpureCalls(rec.outer)) {
        verdict = "no";
        rejectReason = "impure call in inner-loop body";
        reportThis();
        continue;
      }

      // Phase 8 (early): aliasing check.
      if (hasUnsafeAliasing(memops, AA)) {
        verdict = "no";
        rejectReason = "unsafe aliasing between input/output bases";
        reportThis();
        continue;
      }

      stores = collectStores(memops);
      if (stores.empty()) {
        verdict = "no";
        rejectReason = "no stores in nest";
        reportThis();
        continue;
      }
      // outputDeps = union of every store's deps. A nest IV is PARALLEL if it
      // appears in any store; REDUCTION otherwise.
      for (const MemopInfo *s : stores)
        for (const Loop *L : s->deps)
          outputDeps.insert(L);

      // Reduction classification (now non-blocking).
      bool anyReduction = false;
      bool reductionLooksBad = false;
      for (Loop *L : rec.levels) {
        if (outputDeps.count(L) == 0) {
          // No need to require an SSA accumulator phi — for stencil-shape
          // kernels the body is fully unrolled. But if there IS such a loop
          // and it lacks a reduction phi, that's a warning sign; we tolerate.
          if (findReductionInLoop(L)) {
            reductionLoops.insert(L);
            anyReduction = true;
          } else {
            reductionLoops.insert(L); // still classify by subscript
            anyReduction = true;
            reductionLooksBad = true; // best-effort warning, not a reject
          }
        }
      }

      // Body composition.
      countBodyOps(rec.levels.back(), macs, stencilPairs, totalArith,
                   mulOps, addOps);

      // Dependence check on distinct-base memop pairs.
      bool depFail = false;
      for (unsigned i = 0; i < memops.size() && !depFail; ++i) {
        for (unsigned j = i + 1; j < memops.size() && !depFail; ++j) {
          if (memops[i].basePtr == memops[j].basePtr) continue;
          auto dep = DI.depends(memops[i].inst, memops[j].inst, true);
          if (dep && !dep->isLoopIndependent()) {
            depFail = true;
            rejectReason = "loop-carried dependence between distinct bases";
          }
        }
      }
      if (depFail) {
        verdict = "no";
        reportThis();
        continue;
      }

      // Verdict rules (Phase 3, no static score):
      //   yes   = (>=2 parallel) AND (>=1 reduction) AND (>=1 MAC pair) AND
      //           MAC-dominated body (excluding stencil-only kernels).
      //   maybe = passes hard rejects but missing one of those structural
      //           niceties — e.g., 1-D output (matvec), no reduction (stencil),
      //           multi-store kernels, or stencil-only body.
      //   no    = anything that hit a hard reject (handled earlier).
      bool exactWeightedSum = false;
      if (reductionLoops.empty() && stores.size() == 1) {
        if (auto *store = dyn_cast<StoreInst>(stores[0]->inst)) {
          SmallVector<WeightedTerm, 8> terms;
          exactWeightedSum = collectIntegerWeightedTerms(
              store->getValueOperand(), 1, terms) && terms.size() >= 2;
        }
      }

      bool twoParallel = (outputDeps.size() >= 2);
      bool macDominated = (macs > 0) && (2 * macs >= totalArith);
      bool stencilDominated = (stencilPairs > 0) && (macs == 0) &&
                              (2 * stencilPairs >= totalArith);
      if (macs == 0 && stencilPairs == 0 && !exactWeightedSum) {
        verdict = "no";
        rejectReason = "no MAC or stencil-shape compute in body";
        reportThis();
        continue;
      }

      if (twoParallel && anyReduction && macDominated && !reductionLooksBad)
        verdict = "yes";
      else
        verdict = "maybe";

      auto picked = pickHints(rec, memops, stores[0], outputDeps, reductionLoops);
      hints.append(picked.begin(), picked.end());
      if (hints.empty())
        hints.push_back("output_stationary");

      reportThis();
    }

    return PreservedAnalyses::all();
  }
};

MDNode *findLoopProperty(const Loop *loop, StringRef name) {
  MDNode *loopID = loop ? loop->getLoopID() : nullptr;
  if (!loopID)
    return nullptr;
  for (unsigned operandIndex = 1; operandIndex < loopID->getNumOperands();
       ++operandIndex) {
    auto *property = dyn_cast_or_null<MDNode>(loopID->getOperand(operandIndex));
    if (!property || property->getNumOperands() == 0)
      continue;
    auto *propertyName =
        dyn_cast_or_null<MDString>(property->getOperand(0));
    if (propertyName && propertyName->getString() == name)
      return property;
  }
  return nullptr;
}

std::optional<uint64_t> metadataUnsigned(MDNode *node, unsigned operandIndex) {
  if (!node || operandIndex >= node->getNumOperands())
    return std::nullopt;
  auto *constant =
      dyn_cast_or_null<ConstantAsMetadata>(node->getOperand(operandIndex));
  auto *integer = constant ? dyn_cast<ConstantInt>(constant->getValue()) : nullptr;
  if (!integer || integer->isNegative())
    return std::nullopt;
  return integer->getZExtValue();
}

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

struct ParsedTransformRecipe {
  unsigned candidateIndex = 0;
  unsigned dimension0Level = 0;
  uint64_t dimension0TripCount = 0;
  unsigned dimension1Level = 0;
  uint64_t dimension1TripCount = 0;
  unsigned reductionLevel = 0;
  uint64_t reductionTripCount = 0;
};

struct StripSystolicMapMarkerPass
    : public PassInfoMixin<StripSystolicMapMarkerPass> {
  PreservedAnalyses run(Function &function,
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

    function.addFnAttr(SystolicStrippedMarkerAttr,
                       Twine(markerLoop->getLoopDepth()).str());
    for (CallBase *marker : markers)
      marker->eraseFromParent();
    errs() << "Morpher systolic unroll-and-jam temporarily removed "
           << markers.size() << " map marker(s) from " << function.getName()
           << "\n";
    return PreservedAnalyses::none();
  }
};

std::optional<ParsedTransformRecipe> parseTransformRecipe(const Loop *loop) {
  MDNode *property = findLoopProperty(loop, SystolicRecipeMetadata);
  if (!property || property->getNumOperands() != 9)
    return std::nullopt;
  auto schema = metadataUnsigned(property, 1);
  auto candidate = metadataUnsigned(property, 2);
  auto dimension0Level = metadataUnsigned(property, 3);
  auto dimension0TripCount = metadataUnsigned(property, 4);
  auto dimension1Level = metadataUnsigned(property, 5);
  auto dimension1TripCount = metadataUnsigned(property, 6);
  auto reductionLevel = metadataUnsigned(property, 7);
  auto reductionTripCount = metadataUnsigned(property, 8);
  if (!schema || *schema != TransformRecipe::SchemaVersion || !candidate ||
      !dimension0Level || !dimension0TripCount || !dimension1Level ||
      !dimension1TripCount || !reductionLevel || !reductionTripCount)
    return std::nullopt;

  ParsedTransformRecipe recipe;
  recipe.candidateIndex = static_cast<unsigned>(*candidate);
  recipe.dimension0Level = static_cast<unsigned>(*dimension0Level);
  recipe.dimension0TripCount = *dimension0TripCount;
  recipe.dimension1Level = static_cast<unsigned>(*dimension1Level);
  recipe.dimension1TripCount = *dimension1TripCount;
  recipe.reductionLevel = static_cast<unsigned>(*reductionLevel);
  recipe.reductionTripCount = *reductionTripCount;
  return recipe;
}

SmallVector<Loop *, 4> uniqueLoopChain(Loop *outer) {
  SmallVector<Loop *, 4> levels;
  for (Loop *loop = outer; loop != nullptr;) {
    levels.push_back(loop);
    ArrayRef<Loop *> subLoops = loop->getSubLoops();
    if (subLoops.empty())
      break;
    if (subLoops.size() != 1)
      return {};
    loop = subLoops.front();
  }
  return levels;
}

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
struct CanonicalizeSystolicOutputPass
    : public PassInfoMixin<CanonicalizeSystolicOutputPass> {
  PreservedAnalyses run(Function &function, FunctionAnalysisManager &analysisManager) {
    if (function.isDeclaration() ||
        function.getName() != MorpherSystolicTransformFunction)
      return PreservedAnalyses::all();

    LoopInfo &loopInfo = analysisManager.getResult<LoopAnalysis>(function);
    Loop *outer = nullptr;
    for (Loop *candidate : loopInfo.getLoopsInPreorder()) {
      auto parsed = parseTransformRecipe(candidate);
      if (!parsed || parsed->candidateIndex != MorpherSystolicCandidateIndex)
        continue;
      if (outer)
        report_fatal_error("systolic candidate index is ambiguous during output canonicalization");
      outer = candidate;
    }
    if (!outer)
      report_fatal_error("systolic output canonicalization lost the transform recipe");

    SmallVector<Loop *, 4> levels = uniqueLoopChain(outer);
    if (levels.size() != 3)
      report_fatal_error("systolic output canonicalization requires a three-level loop chain");
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
};

struct ConfigureSystolicUnrollJamPass
    : public PassInfoMixin<ConfigureSystolicUnrollJamPass> {
  PreservedAnalyses run(Function &function, FunctionAnalysisManager &analysisManager) {
    if (function.isDeclaration())
      return PreservedAnalyses::all();
    if (MorpherSystolicTransformFunction.empty())
      report_fatal_error("morpher-systolic-fn must name the function to transform");
    if (function.getName() != MorpherSystolicTransformFunction)
      return PreservedAnalyses::all();
    if (MorpherSystolicFactor0 <= 1 || MorpherSystolicFactor1 <= 1)
      report_fatal_error("systolic unroll-and-jam factors must both be greater than one");

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
    if (levels.size() != 3 || recipe.dimension0Level != 0 ||
        recipe.dimension1Level != 1 || recipe.reductionLevel != 2)
      report_fatal_error("systolic transform recipe no longer matches a canonical three-level nest");
    if (loopInfo.getLoopsInPreorder().size() != 3)
      report_fatal_error(
          "systolic transform requires the candidate to be the function's only loop nest");
    Attribute markerDepth = function.getFnAttribute(SystolicStrippedMarkerAttr);
    if (!markerDepth.isStringAttribute())
      report_fatal_error(
          "systolic transform requires please_map_me() in the rolled reduction loop");
    unsigned expectedDepth = 0;
    if (markerDepth.getValueAsString().getAsInteger(10, expectedDepth) ||
        levels[2]->getLoopDepth() != expectedDepth)
      report_fatal_error("please_map_me() did not annotate the rolled reduction loop");

    const uint64_t expectedTrips[] = {
        recipe.dimension0TripCount, recipe.dimension1TripCount,
        recipe.reductionTripCount};
    for (unsigned level = 0; level < levels.size(); ++level) {
      auto currentTrip = constantTripCount(levels[level], scalarEvolution);
      if (!currentTrip || *currentTrip != expectedTrips[level])
        report_fatal_error("systolic transform recipe trip count no longer matches the IR");
    }
    if (recipe.dimension0TripCount % MorpherSystolicFactor0 != 0 ||
        recipe.dimension1TripCount % MorpherSystolicFactor1 != 0)
      report_fatal_error("systolic unroll-and-jam factors must exactly divide their trip counts");
    if (!isSafeToUnrollAndJam(levels[0], scalarEvolution, dominatorTree,
                              dependenceInfo, loopInfo) ||
        !isSafeToUnrollAndJam(levels[1], scalarEvolution, dominatorTree,
                              dependenceInfo, loopInfo))
      report_fatal_error("LLVM rejected unroll-and-jam legality for the selected nest");

    LLVMContext &context = function.getContext();
    Metadata *factor0 = ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(context), MorpherSystolicFactor0));
    Metadata *factor1 = ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(context), MorpherSystolicFactor1));
    setLoopProperty(levels[0], "llvm.loop.unroll_and_jam.count", factor0,
                    /*removeUnrollDisable=*/true);
    setLoopProperty(levels[1], "llvm.loop.unroll_and_jam.count", factor1,
                    /*removeUnrollDisable=*/true);
    setLoopProperty(levels[2], SystolicReductionMetadata, {});

    errs() << "Morpher systolic unroll-and-jam configured: function="
           << function.getName() << " candidate="
           << MorpherSystolicCandidateIndex << " factors="
           << MorpherSystolicFactor0 << "x" << MorpherSystolicFactor1 << "\n";
    return PreservedAnalyses::none();
  }
};

struct RestoreSystolicMapMarkerPass
    : public PassInfoMixin<RestoreSystolicMapMarkerPass> {
  PreservedAnalyses run(Function &function, FunctionAnalysisManager &analysisManager) {
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
};

struct RestoreSystolicMemoryStreamsPass
    : public PassInfoMixin<RestoreSystolicMemoryStreamsPass> {
  PreservedAnalyses run(Function &function, FunctionAnalysisManager &) {
    Attribute bindingsAttr = function.getFnAttribute(SystolicMemStreamBindingsAttr);
    if (!bindingsAttr.isStringAttribute())
      return PreservedAnalyses::all();

    auto baseKey = [](Value *base) -> std::string {
      if (auto *argument = dyn_cast<Argument>(base))
        return "a" + std::to_string(argument->getArgNo());
      if (auto *global = dyn_cast<GlobalValue>(base))
        return "g" + std::to_string(GlobalValue::getGUID(global->getName()));
      if (auto *alloca = dyn_cast<AllocaInst>(base))
        return alloca->getName().empty()
                   ? ""
                   : "l" + std::to_string(
                         GlobalValue::getGUID(alloca->getName()));
      return "";
    };
    SmallVector<std::pair<std::string, std::string>, 4> bindings;
    SmallVector<StringRef, 4> encodedBindings;
    bindingsAttr.getValueAsString().split(encodedBindings, ',', -1, false);
    for (StringRef encoded : encodedBindings) {
      auto split = encoded.split(':');
      if (!split.first.empty() && !split.second.empty())
        bindings.emplace_back(split.first.str(), split.second.str());
    }

    unsigned taggedLoads = 0;
    SmallVector<LoadInst *, 8> rowLoads;
    SmallVector<LoadInst *, 8> columnLoads;
    for (BasicBlock &block : function) {
      for (Instruction &instruction : block) {
        auto *load = dyn_cast<LoadInst>(&instruction);
        if (!load)
          continue;
        Value *base = resolveBasePointer(load->getPointerOperand());
        const std::string key = baseKey(base);
        for (const auto &binding : bindings) {
          if (binding.second != key)
            continue;
          LLVMContext &context = instruction.getContext();
          instruction.setMetadata(
              SystolicMemStreamMetadata,
              MDNode::get(context, MDString::get(context, binding.first)));
          if (binding.first == "row")
            rowLoads.push_back(load);
          else if (binding.first == "column")
            columnLoads.push_back(load);
          ++taggedLoads;
          break;
        }
      }
    }
    function.removeFnAttr(SystolicMemStreamBindingsAttr);
    if (taggedLoads == 0)
      report_fatal_error(
          "systolic transform lost every classified input memory stream");
    if (rowLoads.size() != MorpherSystolicFactor0 ||
        columnLoads.size() != MorpherSystolicFactor1)
      report_fatal_error(
          "systolic transform input stream count does not match jam factors");

    DenseMap<LoadInst *, unsigned> rowIndex;
    DenseMap<LoadInst *, unsigned> columnIndex;
    auto attachIndices = [](ArrayRef<LoadInst *> loads,
                            DenseMap<LoadInst *, unsigned> &indices) {
      for (unsigned index = 0; index < loads.size(); ++index) {
        LoadInst *load = loads[index];
        indices[load] = index;
        LLVMContext &context = load->getContext();
        load->setMetadata(
            SystolicMemStreamIndexMetadata,
            MDNode::get(context, ConstantAsMetadata::get(
                                     ConstantInt::get(Type::getInt32Ty(context),
                                                      index))));
      }
    };
    attachIndices(rowLoads, rowIndex);
    attachIndices(columnLoads, columnIndex);

    auto directLoad = [](Value *value) -> LoadInst * {
      while (auto *cast = dyn_cast<CastInst>(value))
        value = cast->getOperand(0);
      return dyn_cast<LoadInst>(value);
    };
    unsigned taggedMacs = 0;
    for (BasicBlock &block : function) {
      for (Instruction &instruction : block) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (auto *intrinsic = dyn_cast<IntrinsicInst>(&instruction)) {
          Intrinsic::ID id = intrinsic->getIntrinsicID();
          if (id != Intrinsic::fmuladd && id != Intrinsic::fma)
            continue;
          lhs = intrinsic->getOperand(0);
          rhs = intrinsic->getOperand(1);
        } else {
          auto *add = dyn_cast<BinaryOperator>(&instruction);
          if (!add || !isAddShape(add->getOpcode()))
            continue;
          BinaryOperator *product = nullptr;
          for (unsigned operand = 0; operand < 2; ++operand) {
            auto *candidate = dyn_cast<BinaryOperator>(add->getOperand(operand));
            if (candidate && isMulShape(candidate->getOpcode())) {
              product = candidate;
              break;
            }
          }
          if (!product)
            continue;
          lhs = product->getOperand(0);
          rhs = product->getOperand(1);
        }

        LoadInst *first = directLoad(lhs);
        LoadInst *second = directLoad(rhs);
        if (!first || !second)
          continue;
        LoadInst *row = rowIndex.count(first) ? first
                          : rowIndex.count(second) ? second : nullptr;
        LoadInst *column = columnIndex.count(first) ? first
                             : columnIndex.count(second) ? second : nullptr;
        if (!row || !column)
          continue;
        LLVMContext &context = instruction.getContext();
        Metadata *coordinates[] = {
            ConstantAsMetadata::get(ConstantInt::get(
                Type::getInt32Ty(context), rowIndex[row])),
            ConstantAsMetadata::get(ConstantInt::get(
                Type::getInt32Ty(context), columnIndex[column]))};
        instruction.setMetadata(SystolicMacCoordMetadata,
                                MDNode::get(context, coordinates));
        ++taggedMacs;
      }
    }
    const unsigned expectedMacs =
        MorpherSystolicFactor0 * MorpherSystolicFactor1;
    if (taggedMacs != expectedMacs)
      report_fatal_error(
          "systolic transform MAC coordinate count does not match jam tile");
    errs() << "Morpher systolic unroll-and-jam restored " << taggedLoads
           << " indexed input stream tag(s) and " << taggedMacs
           << " MAC coordinate(s) in " << function.getName() << "\n";
    return PreservedAnalyses::none();
  }
};

} // namespace

FunctionPassManager createMorpherSystolicCandidatePipeline() {
  FunctionPassManager FPM;
  FPM.addPass(LoopSimplifyPass());
  FPM.addPass(LCSSAPass());
  FPM.addPass(SystolicCandidatePass(/*emitJSONReport=*/true));
  return FPM;
}

FunctionPassManager createMorpherSystolicUnrollJamPipeline() {
  FunctionPassManager FPM;
  FPM.addPass(LoopSimplifyPass());
  FPM.addPass(LCSSAPass());
  FPM.addPass(SystolicCandidatePass(/*emitJSONReport=*/false));
  FPM.addPass(StripSystolicMapMarkerPass());
  FPM.addPass(CanonicalizeSystolicOutputPass());
  FPM.addPass(ConfigureSystolicUnrollJamPass());
  FPM.addPass(createFunctionToLoopPassAdaptor(LoopUnrollAndJamPass()));
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
