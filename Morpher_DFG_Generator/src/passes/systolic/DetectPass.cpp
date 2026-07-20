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

#include "SystolicInternal.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;
using namespace morpher_systolic;

namespace morpher_systolic {

// Defined once here; declared extern in SystolicInternal.h. Referenced by the
// detector (this TU) and the transform passes.
cl::opt<unsigned> MorpherSystolicCandidateIndex(
    "morpher-systolic-candidate-index", cl::init(0),
    cl::desc("Zero-based systolic candidate index within the target function"));

cl::opt<std::string> MorpherSystolicDataflow(
    "morpher-systolic-dataflow", cl::init("output_stationary"),
    cl::desc("Selected legal systolic dataflow: output_stationary, weight_stationary, or input_stationary"));

} // namespace morpher_systolic

namespace {

// Detector-only cl::opt (report sink). Single TU, keep file-local.
static cl::opt<std::string> MorpherSystolicReport(
    "morpher-systolic-report", cl::init(""),
    cl::desc("Write SystolicCandidatePass JSON report to this file (default: stderr)"));

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
  if (!recipe.supported || recipe.parallelDimensions.empty() ||
      recipe.parallelDimensions.size() > 2 ||
      recipe.reductionLevel >= rec.levels.size())
    return;
  const unsigned rowLevel = recipe.parallelDimensions[0].level;
  const bool isOneDimensional = recipe.parallelDimensions.size() == 1;
  const unsigned columnLevel = isOneDimensional
      ? recipe.reductionLevel
      : recipe.parallelDimensions[1].level;
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
    if (memop.deps.count(rowLoop) &&
        (isOneDimensional || !memop.deps.count(columnLoop)))
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
  rec.outer->getHeader()->getParent()->addFnAttr(
      SystolicDataflowAttr, MorpherSystolicDataflow);
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
  const bool isGemm = family == "gemm_like_2d_reduction";
  const bool isDot = family == "dot_1d_reduction";
  if (!isGemm && !isDot)
    return unsupported("lowering family is not a supported reduction contraction");
  const unsigned expectedLevels = isGemm ? 3 : 2;
  if (rec.levels.size() != expectedLevels)
    return unsupported(isGemm
                           ? "v1 requires a canonical perfect three-level nest"
                           : "v1 requires a canonical perfect two-level nest");
  for (unsigned level = 0; level + 1 < rec.levels.size(); ++level) {
    ArrayRef<Loop *> subLoops = rec.levels[level]->getSubLoops();
    if (subLoops.size() != 1 || subLoops.front() != rec.levels[level + 1])
      return unsupported(isGemm
                             ? "v1 requires a canonical perfect three-level nest"
                             : "v1 requires a canonical perfect two-level nest");
  }
  if (!rec.levels.back()->getSubLoops().empty())
    return unsupported(isGemm
                           ? "v1 requires a canonical perfect three-level nest"
                           : "v1 requires a canonical perfect two-level nest");
  if (stores.size() != 1)
    return unsupported("v1 requires exactly one output base");
  if (reductionLoops.size() != 1)
    return unsupported("v1 requires exactly one reduction loop");

  const Loop *reduction = *reductionLoops.begin();
  if (rec.levels.back() != reduction)
    return unsupported("v1 requires the reduction loop to be innermost");
  const unsigned expectedParallel = expectedLevels - 1;
  if (outputDeps.size() != expectedParallel)
    return unsupported(isGemm
                           ? "v1 requires the two outer loops to be parallel output dimensions"
                           : "v1 requires one outer parallel output dimension");
  for (unsigned level = 0; level < expectedParallel; ++level)
    if (!outputDeps.count(rec.levels[level]))
      return unsupported(isGemm
                             ? "v1 requires the two outer loops to be parallel output dimensions"
                             : "v1 requires one outer parallel output dimension");

  for (unsigned level = 0; level < rec.levels.size(); ++level) {
    auto tripCount = constantTripCount(rec.levels[level], SE);
    if (!tripCount || *tripCount == 0)
      return unsupported("v1 requires constant positive trip counts");
    if (level < expectedParallel) {
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

bool containsMapMarker(const Loop *outer) {
  if (!outer)
    return false;
  for (BasicBlock *block : outer->blocks()) {
    for (const Instruction &instruction : *block) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (!call)
        continue;
      const Value *called = call->getCalledOperand()->stripPointerCasts();
      const auto *function = dyn_cast<Function>(called);
      if (function && function->getName() == "please_map_me")
        return true;
    }
  }
  return false;
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
  OS << ",\"contains_map_marker\":"
     << (containsMapMarker(rec.outer) ? "true" : "false");
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

} // namespace

namespace morpher_systolic {

// ----------------------------------------------------------------------------
// Main pass driver
// ----------------------------------------------------------------------------

PreservedAnalyses SystolicCandidatePass::run(Function &F, FunctionAnalysisManager &AM) {
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
          if (!emitJSONReport &&
              candidateIndex == MorpherSystolicCandidateIndex) {
            if (MorpherSystolicDataflow != "output_stationary" &&
                MorpherSystolicDataflow != "weight_stationary" &&
                MorpherSystolicDataflow != "input_stationary")
              report_fatal_error(Twine("unsupported selected systolic dataflow: ") +
                                 MorpherSystolicDataflow);
            if (std::find(hints.begin(), hints.end(), MorpherSystolicDataflow) ==
                hints.end())
              report_fatal_error(Twine("selected systolic dataflow is not legal for the candidate: ") +
                                 MorpherSystolicDataflow);
            attachSystolicMemoryStreams(rec, memops, stores, recipe);
          }
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

} // namespace morpher_systolic

FunctionPassManager createMorpherSystolicCandidatePipeline() {
  FunctionPassManager FPM;
  FPM.addPass(LoopSimplifyPass());
  FPM.addPass(LCSSAPass());
  FPM.addPass(SystolicCandidatePass(/*emitJSONReport=*/true));
  return FPM;
}
