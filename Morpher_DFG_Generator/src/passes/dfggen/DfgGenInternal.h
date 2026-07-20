#pragma once

// Shared declarations for the mechanically partitioned DFG-generation pass.
// Existing global state is intentionally preserved in DfgGenDriver.cpp; this
// refactor changes translation-unit ownership, not lifetime or behavior.

#include "llvm/Analysis/LoopPass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/PassManager.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"

#include "llvm/Transforms/Utils.h"

/* #include "llvm/Transforms/Scalar.h" */
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GraphWriter.h"

#include "llvm/ADT/GraphTraits.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/Passes.h"

#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"

#include "llvm/IR/Attributes.h"

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>

//My Classes
#include <morpherdfggen/common/dfg.h>
#include <morpherdfggen/common/dfgnode.h>
#include <morpherdfggen/dfg/DFGPartPred.h>
#include <morpherdfggen/common/edge.h>
#include <morpherdfggen/common/DFGError.h>
#include <morpherdfggen/util/llvm_compat.h>


#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

#define DEFAULT_DATA_PLACEMENT
#define LV_NAME "dfg_gen"
#define DEBUG_TYPE LV_NAME

struct MappingUnit {
  bool isInnerLoop = false;
  Loop *lp;
  std::set<BasicBlock *> allBlocks;
  std::set<std::pair<BasicBlock *, BasicBlock *>> entryBlocks;
  std::set<std::pair<BasicBlock *, BasicBlock *>> exitBlocks;
};

struct LoopTree {
  Loop *lp;
  std::vector<LoopTree> lpChildren;
};

extern cl::opt<unsigned> banks_number;
extern cl::opt<unsigned> bank_size;
extern cl::opt<unsigned> dp_policy;
extern cl::opt<bool> SuppressDFGDot;

extern std::map<std::string, int> sizeArrMap;
extern std::map<const BasicBlock *, std::vector<const BasicBlock *>>
    BBSuccBasicBlocks;
extern std::map<Loop *, std::vector<BasicBlock *>> loopsExclusieBasicBlockMap;
extern std::map<std::string, MappingUnit> mappingUnitMap;
extern std::map<std::string, Loop *> mappingUnit2LoopMap;
extern std::map<BasicBlock *, std::string> BB2MUnitMap;
extern std::set<BasicBlock *> nonloopBBs;
extern LoopTree rootLoop;
extern std::vector<munitTransition> munitTransitions;
extern std::vector<munitTransition> munitTransitionsALL;

inline Function *resolveCalledFunction(CallBase *call) {
  if (!call)
    return nullptr;
  if (Function *called = call->getCalledFunction())
    return called;
  return dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
}

inline unsigned getPointerDepth(Type *T) {
  if (!T->isPointerTy())
    return 0;
  return 1;
}

inline unsigned getEffectiveArrayDims(const GetElementPtrInst *GEP) {
  unsigned dims = GEP->getNumIndices();
  if (dims == 0)
    return 0;
  if (const ConstantInt *CI = dyn_cast<ConstantInt>(GEP->idx_begin()->get()))
    if (CI->isZero())
      dims--;
  return dims;
}

void ValidateSupportedArrayDimensions(Function &function);
void HoistInvariantRowPointers(Function &function, LoopInfo &loopInfo);
void FlattenDoublePointers(Function &function, LoopInfo &loopInfo);
void ParseSizeAttr(Function &function,
                   std::map<std::string, int> *arraySizes);
void RemoveSelectLeafs(Function &function);
bool ExpandConstantFunnelShifts(Function &function);
bool ExpandIntegerMinMaxIntrinsics(Function &function);
void InsertORtoSingularConditionalBB(Function &function);
void ReplaceCMPs(Function &function);

void traverseDefTree(
    Instruction *instruction, int depth, DFG *dfg,
    std::map<Instruction *, int> *instructionMap,
    std::map<const BasicBlock *, std::vector<const BasicBlock *>> successors,
    std::set<BasicBlock *> validBlocks, MemoryDependenceAnalysis *MD = nullptr);
void printDFGDOT(std::string fileName, DFG *dfg);
void dfsBB(
    SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 1> backEdges,
    std::map<const BasicBlock *, std::vector<const BasicBlock *>> *successors,
    BasicBlock *current, const BasicBlock *start);
void printBBSuccMap(
    Function &function,
    std::map<const BasicBlock *, std::vector<const BasicBlock *>> successors);
void populateNonLoopBBs(Function &function, std::vector<Loop *> loops);
void getInnerMostLoops(std::vector<Loop *> *innerMostLoops,
                       std::vector<Loop *> loops,
                       std::map<Loop *, std::string> *loopNames,
                       std::string loopName, LoopTree *parent);
void printLoopTree(LoopTree root, std::map<Loop *, std::string> *loopNames,
                   int tabs = 0);
void printMappableUnitMap();
void populateBBTrans();
std::string getMappingUnitNameUsingTokenFunction(Function &function);
std::string findMappingUnitForOptionalMarker(Function &function,
                                             const std::string &markerName);

void AllocateSPMBanks(
    std::unordered_set<Value *> &outerValues,
    std::unordered_map<Value *, GetElementPtrInst *> &memoryPointers,
    std::unordered_map<Value *, int> &accessCounts,
    std::unordered_map<Value *, SPM_BANK> &bankAllocation,
    std::unordered_map<Value *, int> &baseAddresses, Function &function,
    unsigned &requiredBankSize, unsigned &effectiveBankSize);
