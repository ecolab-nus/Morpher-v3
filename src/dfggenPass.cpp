#include "llvm/Pass.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"

#include "llvm/Transforms/Utils.h"

#include "llvm/Transforms/Scalar.h"
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
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/Support/CommandLine.h"

#include "llvm/ADT/GraphTraits.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/Passes.h"

#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
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

//#define CDFG
#define DEFAULT_DATA_PLACEMENT

AttributeList attr21;

static bool xmlRun = false;

int MEM_SIZE;

namespace llvm
{
void initializeSkeletonFunctionPassPass(PassRegistry &);
void initializeSkeletonModulePassPass(PassRegistry &);

Pass *createskeleton();
} // namespace llvm

using namespace llvm;
#define LV_NAME "dfg_gen" //"sfp"
#define DEBUG_TYPE LV_NAME

//static cl::opt<unsigned> loopNumber("ln", cl::init(0), cl::desc("The loop number to map"));
// static cl::opt<std::string> munitName("munit", cl::init("na"), cl::desc("the mapping unit name, e.g. : PRE_LN11, INNERMOST_LN11"));
static cl::opt<std::string> fName("fn", cl::init("na"), cl::desc("the function name"));
// static cl::opt<bool> noName("nn", cl::desc("map all functions and loops"));
// static cl::opt<unsigned> initMII("ii", cl::init(0), cl::desc("The starting II for the mapping"));
// static cl::opt<unsigned> dimX("dx", cl::init(4), cl::desc("DimX"));
// static cl::opt<unsigned> dimY("dy", cl::init(4), cl::desc("DimY"));

static cl::opt<unsigned> banks_number ("nobanks", cl::init(2), cl::desc("number of SPM banks"));

static cl::opt<unsigned> bank_size ("banksize", cl::init(2048), cl::desc("number of bytes in a bank"));
static cl::opt<unsigned> dp_policy ("dppolicy", cl::init(0), cl::desc("data placement policy"));

STATISTIC(LoopsAnalyzed, "Number of loops analyzed for vectorization");

static std::map<std::string, int> sizeArrMap;
static std::set<BasicBlock *> LoopBB;
static std::map<const BasicBlock *, std::vector<const BasicBlock *>> BBSuccBasicBlocks;
static std::map<Loop *, std::vector<BasicBlock *>> loopsExclusieBasicBlockMap;

typedef struct
{
	bool isInnerLoop = false;
	Loop *lp;
	std::set<BasicBlock *> allBlocks;
	std::set<std::pair<BasicBlock *, BasicBlock *>> entryBlocks;
	std::set<std::pair<BasicBlock *, BasicBlock *>> exitBlocks;
} MappingUnit;

static std::map<std::string, MappingUnit> mappingUnitMap;
static std::map<std::string, Loop *> mappingUnit2LoopMap;
static std::map<BasicBlock *, std::string> BB2MUnitMap;
std::set<BasicBlock *> nonloopBBs;

typedef struct LoopTree LoopTree;
struct LoopTree
{
	Loop *lp;
	std::vector<LoopTree> lpChildren;
};
LoopTree rootLoop; //current loop is NULL is just a struct capture all the toplevel loops.

std::vector<munitTransition> munitTransitions;
std::vector<munitTransition> munitTransitionsALL;

void traverseDefTree(Instruction *I,
		int depth,
		DFG *currBBDFG, std::map<Instruction *, int> *insMapIn,
		std::map<const BasicBlock *, std::vector<const BasicBlock *>> BBSuccBasicBlocks,
		std::set<BasicBlock *> validBB,
		MemoryDependenceAnalysis *MD = NULL)
{

	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 1> BackEdgesBB;
	FindFunctionBackedges(*(I->getFunction()), BackEdgesBB);

	(*insMapIn)[I]++;
	dfgNode curr(I, currBBDFG);
	currBBDFG->InsertNode(I);
	dfgNode *currPtr = currBBDFG->findNode(I);

	if (!dyn_cast<PHINode>(I))
	{
		for (Use &V : I->operands())
		{
			if (Instruction *ParIns = dyn_cast<Instruction>(V))
			{
				if (validBB.find(ParIns->getParent()) == validBB.end())
				{
					currBBDFG->findNode(I)->addLoadParent(ParIns);
				}
			}

			if (Argument *arg = dyn_cast<Argument>(V))
			{
				LLVM_DEBUG(dbgs() << "Argument adding load parent : ");
				LLVM_DEBUG(V->dump());

				if (!V->getType()->isPointerTy())
				{
					currBBDFG->findNode(I)->addLoadParent(arg);
				}
			}
		}
	}

	for (User *U : I->users())
	{

		LLVM_DEBUG(dbgs() << "I :");
		LLVM_DEBUG(I->dump());
		LLVM_DEBUG(dbgs() << "Inst : ");
		LLVM_DEBUG(U->dump());

		if (Instruction *Inst = dyn_cast<Instruction>(U))
		{
			LLVM_DEBUG(dbgs() << "#####TRAVDEFTREE :: Inst Valid!\n");

			//Searching inside basicblocks of the loop
			if (validBB.find(Inst->getParent()) == validBB.end())
			{
				currBBDFG->findNode(I)->addStoreChild(I);
				continue;
			}

			if (Inst->getOpcode() == Instruction::PHI)
			{
				LLVM_DEBUG(dbgs() << "#####TRAVDEFTREE :: PHI Child found2!\n");
				currBBDFG->findNode(I)->addPHIchild(Inst);
				LLVM_DEBUG(I->dump());
				LLVM_DEBUG(Inst->dump());
				continue;
			}

			if (std::find(BBSuccBasicBlocks[I->getParent()].begin(), BBSuccBasicBlocks[I->getParent()].end(), Inst->getParent()) == BBSuccBasicBlocks[I->getParent()].end())
			{
				if (Inst->getOpcode() == Instruction::PHI)
				{
					LLVM_DEBUG(dbgs() << "#####TRAVDEFTREE :: PHI Child found1!\n");
					currBBDFG->findNode(I)->addPHIchild(Inst);
				}
				LLVM_DEBUG(dbgs() << "#####TRAVDEFTREE :: backedge found!\n");
				continue;
			}

			std::pair<const BasicBlock *, const BasicBlock *> bbCouple(I->getParent(), Inst->getParent());
			if (std::find(BackEdgesBB.begin(), BackEdgesBB.end(), bbCouple) != BackEdgesBB.end())
			{
				if (I->getParent() != Inst->getParent())
				{
					LLVM_DEBUG(dbgs() << "#####TRAVDEFTREE :: backedge found!\n");
					continue;
				}
			}

			if (Inst->getOpcode() == Instruction::PHI)
			{
				if (I->getParent() == Inst->getParent())
				{
					LLVM_DEBUG(dbgs()  << "#####TRAVDEFTREE :: PHI Child found2!\n");
					currBBDFG->findNode(I)->addPHIchild(Inst);
					LLVM_DEBUG(I->dump());
					LLVM_DEBUG(Inst->dump());
					continue;
				}
			}

			currBBDFG->findNode(I)->addChild(Inst);

			if (insMapIn->find(Inst) == insMapIn->end())
			{
				traverseDefTree(Inst, depth + 1, currBBDFG, insMapIn, BBSuccBasicBlocks, validBB);
			}

			currBBDFG->findNode(Inst)->addAncestor(I);
		}
	}
}

void printDFGDOT(std::string fileName, DFG *currBBDFG)
{
	std::ofstream ofs;
	ofs.open(fileName.c_str());
	dfgNode *node;
	int count = 0;

	//Write the initial info
	ofs << "digraph Region_18 {\n\tgraph [ nslimit = \"1000.0\",\n\torientation = landscape,\n\t\tcenter = true,\n\tpage = \"8.5,11\",\n\tsize = \"10,7.5\" ] ;" << std::endl;

	assert(currBBDFG->getNodes().size() != 0);

	if (currBBDFG->getNodes()[0]->getNode() == NULL)
	{
	}

	for (int i = 0; i < currBBDFG->getNodes().size(); i++)
	{
		node = currBBDFG->getNodes()[i];

		if (node->getNode() == NULL)
		{
		}

		ofs << "\"Op_" << node->getIdx() << "\" [ fontname = \"Helvetica\" shape = box, label = \" ";

		if (node->getNode() != NULL)
		{
			ofs << node->getNode()->getOpcodeName();

			if (node->hasConstantVal())
			{
				ofs << " C="
						<< "0x" << std::hex << node->getConstantVal() << std::dec;
			}

			if (node->isGEP())
			{
				ofs << " C="
						<< "0x" << std::hex << node->getGEPbaseAddr() << std::dec;
			}

			ofs << " BB" << node->getNode()->getParent()->getName().str();
		}
		else
		{
			ofs << node->getNameType();
			if (node->isOutLoop())
			{
				ofs << " C="
						<< "0x" << node->getoutloopAddr() << std::dec;
			}

			if (node->hasConstantVal())
			{
				ofs << " C="
						<< "0x" << node->getConstantVal() << std::dec;
			}
		}

		if (node->getFinalIns() != NOP)
		{
			ofs << " HyIns=" << currBBDFG->HyCUBEInsStrings[node->getFinalIns()];
		}

		if (node->getMappedLoc() != NULL)
		{
			ofs << ",\n"
					<< node->getIdx() << ", ASAP=" << node->getASAPnumber()
					<< ", ALAP=" << node->getALAPnumber()
					<< ", (t,y,x)=(" << node->getMappedLoc()->getT() << "," << node->getMappedLoc()->getY() << "," << node->getMappedLoc()->getX() << ")"
					<< ",RT=" << node->getmappedRealTime()
					<< "\"]" << std::endl;
		}
		else
		{
			ofs << ",\n"
					<< node->getIdx() << ", ASAP=" << node->getASAPnumber()
					<< ", ALAP=" << node->getALAPnumber()
					<< "\"]" << std::endl;
		}
	}

	ofs << "{ rank = same ;\n}" << std::endl;

	for (int i = 0; i < currBBDFG->getNodes().size(); i++)
	{
		node = currBBDFG->getNodes()[i];

		int j;
		for (j = 0; j < node->getChildren().size(); j++)
		{
			assert(currBBDFG->findEdge(node, node->getChildren()[j]) != NULL);
			if (currBBDFG->findEdge(node, node->getChildren()[j])->getType() == EDGE_TYPE_DATA)
			{
				ofs << "\"Op_" << node->getIdx() << "\" -> \"Op_" << node->getChildren()[j]->getIdx() << "\" [style = bold, color = red];" << std::endl;
			}
			else if (currBBDFG->findEdge(node, node->getChildren()[j])->getType() == EDGE_TYPE_CTRL)
			{
				ofs << "\"Op_" << node->getIdx() << "\" -> \"Op_" << node->getChildren()[j]->getIdx() << "\" [style = bold, color = black];" << std::endl;
			}
		}

		//adding recurrence edges
		for (j = 0; j < node->getRecChildren().size(); j++)
		{
			assert(currBBDFG->findEdge(node, node->getRecChildren()[j]) != NULL);
			if (currBBDFG->findEdge(node, node->getRecChildren()[j])->getType() == EDGE_TYPE_LDST)
			{
				ofs << "\"Op_" << node->getIdx() << "\" -> \"Op_" << node->getRecChildren()[j]->getIdx() << "\" [style = bold, color = green];" << std::endl;
			}
		}

		//adding phi edges
		for (j = 0; j < node->getPHIchildren().size(); j++)
		{
			assert(currBBDFG->findEdge(node, node->getPHIchildren()[j]) != NULL);
			if (currBBDFG->findEdge(node, node->getPHIchildren()[j])->getType() == EDGE_TYPE_PHI)
			{
				ofs << "\"Op_" << node->getIdx() << "\" -> \"Op_" << node->getPHIchildren()[j]->getIdx() << "\" [style = bold, color = orange];" << std::endl;
			}
		}
	}

	ofs << "}" << std::endl;
	ofs.close();
}

Instruction *checkMemDepedency(Instruction *I, MemoryDependenceResults *MD)
{
	MemDepResult mRes;
	//errs() << "#*#*#*#*#* This is a memory op #*#*#*#*#*\n";
	mRes = MD->getDependency(I);

	if (mRes.getInst() != NULL)
	{
		//errs() << "Dependency : \n";
		LLVM_DEBUG(mRes.getInst()->dump());
	}
	else
	{
		//errs() << "Not Dependent or cannot find the dependence : \n";
	}

	return mRes.getInst();
}

void dfsBB(SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 1> BackEdgesBB,
		std::map<const BasicBlock *, std::vector<const BasicBlock *>> *BBSuccBasicBlocksPtr,
		BasicBlock *currBB,
		const BasicBlock *startBB)
{
	LLVM_DEBUG(dbgs()  << "currBB : " << currBB->getName() << "\n");

	succ_iterator SI(succ_begin(currBB)), SE(succ_end(currBB));
	for (; SI != SE; ++SI)
	{
		BasicBlock *succ = *SI;

		std::pair<const BasicBlock *, const BasicBlock *> bbCouple(currBB, succ);
		if (std::find(BackEdgesBB.begin(), BackEdgesBB.end(), bbCouple) != BackEdgesBB.end())
		{
			continue;
		}

		if (std::find((*BBSuccBasicBlocksPtr)[startBB].begin(), (*BBSuccBasicBlocksPtr)[startBB].end(), succ) != (*BBSuccBasicBlocksPtr)[startBB].end())
		{
			continue;
		}

		(*BBSuccBasicBlocksPtr)[startBB].push_back(succ);
		dfsBB(BackEdgesBB, BBSuccBasicBlocksPtr, succ, startBB);
	}
	return;
}

void printBBSuccMap(Function &F,
		std::map<const BasicBlock *, std::vector<const BasicBlock *>> BBSuccBasicBlocks)
{

	std::map<const BasicBlock *, std::vector<const BasicBlock *>>::iterator it;
	std::ofstream basicblockmapfile;
	std::string fname = F.getName().str() + "_basicblockmapfile.log";
	basicblockmapfile.open(fname.c_str());
	for (it = BBSuccBasicBlocks.begin(); it != BBSuccBasicBlocks.end(); it++)
	{
		basicblockmapfile << "BB::" << it->first->getName().str() << " = ";
		for (int u = 0; u < it->second.size(); ++u)
		{
			basicblockmapfile << it->second[u]->getName().str() << ", ";
		}
		basicblockmapfile << "\n";
	}
	basicblockmapfile.close();
}

void populateNonLoopBBs(Function &F, std::vector<Loop *> loops)
{
	for (BasicBlock &BB : F)
	{

		BasicBlock *BBPtr = cast<BasicBlock>(&BB);
		nonloopBBs.insert(BBPtr);
	}

	for (Loop *lp : loops)
	{
		for (BasicBlock *BB : lp->getBlocks())
		{
			nonloopBBs.erase(BB);
		}
	}
}

void getInnerMostLoops(std::vector<Loop *> *innerMostLoops, std::vector<Loop *> loops, std::map<Loop *, std::string> *loopNames, std::string lnstr, LoopTree *parentLoopTree)
{
	for (int i = 0; i < loops.size(); ++i)
	{
		std::stringstream ss;
		ss << lnstr << std::hex << (i + 1) << std::dec;
		(*loopNames)[loops[i]] = ss.str();

		LoopTree currLPTree;
		currLPTree.lp = loops[i];
		parentLoopTree->lpChildren.push_back(currLPTree);

		LLVM_DEBUG( dbgs() << "LoopName : " << ss.str() << "\n");
		for (Loop::block_iterator bb = loops[i]->block_begin(); bb != loops[i]->block_end(); ++bb)
		{
			LLVM_DEBUG( dbgs() << (*bb)->getName() << ",");
		}
		LLVM_DEBUG( dbgs() << "; EXIT=");
		SmallVector<BasicBlock *, 8> loopExitBlocks;
		loops[i]->getExitBlocks(loopExitBlocks);
		for (int i = 0; i < loopExitBlocks.size(); ++i)
		{
			LLVM_DEBUG( dbgs() << loopExitBlocks[i]->getName() << ",");
		}

		LLVM_DEBUG( dbgs() << "\n");

		if (loops[i]->getSubLoops().size() == 0)
		{
			innerMostLoops->push_back(loops[i]);
			mappingUnitMap["INNERMOST_" + ss.str()].isInnerLoop = true;
			mappingUnitMap["INNERMOST_" + ss.str()].lp = loops[i];

			for (BasicBlock *BB : loops[i]->getBlocks())
			{
				loopsExclusieBasicBlockMap[loops[i]].push_back(BB);
				mappingUnitMap["INNERMOST_" + ss.str()].allBlocks.insert(BB);
				BB2MUnitMap[BB] = "INNERMOST_" + ss.str();
			}
		}
		else
		{
			getInnerMostLoops(innerMostLoops, loops[i]->getSubLoops(), loopNames, ss.str(), &parentLoopTree->lpChildren.back());

			for (BasicBlock *BB : loops[i]->getBlocks())
			{
				bool BBfound = false;
				for (std::pair<Loop *, std::vector<BasicBlock *>> pair : loopsExclusieBasicBlockMap)
				{
					if (std::find(pair.second.begin(), pair.second.end(), BB) != pair.second.end())
					{
						BBfound = true;
						break;
					}
				}
				if (!BBfound)
				{
					loopsExclusieBasicBlockMap[loops[i]].push_back(BB);
				}
			}
		}
	}
}

void printNtabs(int N)
{
	for (int i = 0; i < N; ++i)
	{
		LLVM_DEBUG( dbgs() << "\t");
	}
}

void printLoopTree(LoopTree rootLoop, std::map<Loop *, std::string> *loopNames, int tabs = 0);
void printLoopTree(LoopTree rootLoop, std::map<Loop *, std::string> *loopNames, int tabs)
{

	struct less_than_lp
	{
		inline bool operator()(const LoopTree &lptree1, const LoopTree &lptree2)
		{
			BasicBlock *lp1Header = lptree1.lp->getHeader();
			BasicBlock *lp2Header = lptree2.lp->getHeader();
			if (std::find(BBSuccBasicBlocks[lp1Header].begin(),
					BBSuccBasicBlocks[lp1Header].end(),
					lp2Header) != BBSuccBasicBlocks[lp1Header].end())
			{
				//this means lp2 is a succesor of lp1
				return true;
			}
			return false;
		}
	};
	std::sort(rootLoop.lpChildren.begin(), rootLoop.lpChildren.end(), less_than_lp());

	std::vector<BasicBlock *> thisLoopBB;

	if (rootLoop.lp == NULL)
	{
		for (BasicBlock *BB : nonloopBBs)
		{
			thisLoopBB.push_back(BB);
		}
	}
	else
	{
		thisLoopBB = loopsExclusieBasicBlockMap[rootLoop.lp];
	}

	if (false)
	{
		LLVM_DEBUG(dbgs() << "ROOT LOOP\n");
	}
	else
	{
		printNtabs(tabs);
		if (rootLoop.lp != NULL)
		{
			LLVM_DEBUG(dbgs() << (*loopNames)[rootLoop.lp] << "******begin\n");
		}

		for (BasicBlock *bb : thisLoopBB)
		{
			printNtabs(tabs);
			LLVM_DEBUG(dbgs() << bb->getName() << ",");
			bool found = false;
			for (LoopTree lt : rootLoop.lpChildren)
			{
				std::stringstream ss;
				BasicBlock *lpHeader = lt.lp->getHeader();
				if (std::find(BBSuccBasicBlocks[bb].begin(),
						BBSuccBasicBlocks[bb].end(),
						lpHeader) != BBSuccBasicBlocks[bb].end())
				{
					LLVM_DEBUG(dbgs() << "PRE_" << (*loopNames)[lt.lp] << ",");
					ss << "PRE_" << (*loopNames)[lt.lp];
					mappingUnitMap[ss.str()].allBlocks.insert(bb);
					mappingUnitMap[ss.str()].lp = rootLoop.lp;
					BB2MUnitMap[bb] = ss.str();
					found = true;
					break;
				}
			}
			if (found)
			{
				LLVM_DEBUG(dbgs() << "\n");
				continue;
			}

			std::vector<LoopTree> reverseLpChildren = rootLoop.lpChildren;
			std::reverse(reverseLpChildren.begin(), reverseLpChildren.end());
			for (LoopTree lt : reverseLpChildren)
			{
				std::stringstream ss;
				BasicBlock *lpHeader = lt.lp->getHeader();
				if (std::find(BBSuccBasicBlocks[lpHeader].begin(),
						BBSuccBasicBlocks[lpHeader].end(),
						bb) != BBSuccBasicBlocks[lpHeader].end())
				{
					LLVM_DEBUG(dbgs() << "POST_" << (*loopNames)[lt.lp] << ",");
					ss << "POST_" << (*loopNames)[lt.lp];
					mappingUnitMap[ss.str()].allBlocks.insert(bb);
					mappingUnitMap[ss.str()].lp = rootLoop.lp;
					BB2MUnitMap[bb] = ss.str();
					found = true;
					break;
				}
			}
			LLVM_DEBUG(dbgs() << "\n");
		}

		for (std::pair<std::string, MappingUnit> pair : mappingUnitMap)
		{
			//for entry blocks
			std::string name = pair.first;
			for (BasicBlock *BB : mappingUnitMap[name].allBlocks)
			{
				for (auto it = pred_begin(BB), et = pred_end(BB); it != et; ++it)
				{
					BasicBlock *predBB = *it;
					if (std::find(mappingUnitMap[name].allBlocks.begin(),
							mappingUnitMap[name].allBlocks.end(),
							predBB) == mappingUnitMap[name].allBlocks.end())
					{
						mappingUnitMap[name].entryBlocks.insert(std::make_pair(predBB, BB));
					}
				}
			}

			//for exit blocks
			for (BasicBlock *BB : mappingUnitMap[name].allBlocks)
			{
				for (auto it = succ_begin(BB), et = succ_end(BB); it != et; ++it)
				{
					BasicBlock *succBB = *it;
					if (std::find(mappingUnitMap[name].allBlocks.begin(),
							mappingUnitMap[name].allBlocks.end(),
							succBB) == mappingUnitMap[name].allBlocks.end())
					{
						mappingUnitMap[name].exitBlocks.insert(std::make_pair(BB, succBB));
					}
				}
			}
		}

		printNtabs(tabs);
		if (rootLoop.lp != NULL)
		{
			LLVM_DEBUG(dbgs() << (*loopNames)[rootLoop.lp] << "******end\n");
		}
	}

	for (LoopTree child : rootLoop.lpChildren)
	{
		printLoopTree(child, loopNames, tabs + 1);
	}


}

void printMappableUnitMap()
{

	LLVM_DEBUG(dbgs() << "Printing mappable unit map ... \n");

	//outs() << "Printing mappable unit map ... \n";
	for (std::pair<std::string, MappingUnit> pair : mappingUnitMap)
	{
		//outs() << pair.first << " :: ";
		LLVM_DEBUG(dbgs() << pair.first << " :: ");
		for (BasicBlock *bb : pair.second.allBlocks)
		{
			LLVM_DEBUG(dbgs() << bb->getName() << ",");
		}
		//		outs() << "|entry=";
		LLVM_DEBUG(dbgs() << "|entry=");
		for (std::pair<BasicBlock *, BasicBlock *> bbPair : pair.second.entryBlocks)
		{
			LLVM_DEBUG(dbgs()  << bbPair.first->getName() << "to" << bbPair.second->getName() << ",");
		}
		//		outs() << "|exit=";
		LLVM_DEBUG(dbgs() <<"|exit=");
		for (std::pair<BasicBlock *, BasicBlock *> bbPair : pair.second.exitBlocks)
		{
			LLVM_DEBUG(dbgs() << bbPair.first->getName() << "to" << bbPair.second->getName() << ",");
		}
		//		outs() << "\n";
		LLVM_DEBUG(dbgs() <<"\n");
	}
}

std::string findMUofBB(BasicBlock *BB)
{
	for (std::pair<std::string, MappingUnit> pair1 : mappingUnitMap)
	{
		if (pair1.second.allBlocks.find(BB) != pair1.second.allBlocks.end())
		{
			return pair1.first;
		}
	}
	return "FUNC_BODY";
}

void dfsmunitTrans(std::pair<std::string, std::string> currEdge,
		std::vector<std::string> currPath,
		std::map<std::string, std::map<std::string, std::set<std::string>>> &transbasedScalarTransfers,
		std::map<std::string, std::map<std::string, std::set<std::vector<std::string>>>> &transbasedNextMunit,
		std::map<std::string, std::string> varOwner,
		std::set<std::pair<std::string, std::string>> visitedEdges,
		std::map<std::string, std::set<std::string>> &munitTrans,
		std::map<std::string, std::set<std::string>> &varNeeds,
		int tabs = 0)
{

	if (visitedEdges.find(currEdge) != visitedEdges.end())
	{
		return;
	}

	std::string currMunit = currEdge.second;
	for (int i = 0; i < tabs; ++i)
	{
		LLVM_DEBUG(dbgs() << "\t");
	}
	LLVM_DEBUG(dbgs()  << "prevMunit=" << currEdge.first << ",currMunit=" << currMunit << "\n");

	for (std::string varName : varNeeds[currMunit])
	{
		assert(varOwner.find(varName) != varOwner.end());
		assert(!varOwner[varName].empty());

		for (int i = 0; i < tabs; ++i)
		{
			LLVM_DEBUG(dbgs()  << "\t");
		}
		LLVM_DEBUG(dbgs()  << "varName = " << varName << ",");
		LLVM_DEBUG(dbgs()  << "PrevOwner =" << varOwner[varName] << ",");

		if (varOwner[varName].compare(currMunit) == 0)
		{
			LLVM_DEBUG(dbgs() << "isOwned by itself\n");
			continue;
		}
		assert(varOwner[varName].compare(currMunit) != 0);

		bool found = false;
		int foundi = 0;
		LLVM_DEBUG(dbgs() << "currPath.size=" << currPath.size() << ",");
		std::vector<std::string> connectingPath;
		for (int i = 0; i < currPath.size(); ++i)
		{
			std::string nextMunit;
			if (i == currPath.size() - 1)
			{
				nextMunit = currMunit;
			}
			else
			{
				nextMunit = currPath[i + 1];
			}

			if (found)
			{
				if (connectingPath.back().compare(nextMunit) != 0)
				{
					connectingPath.push_back(nextMunit);
				}

				LLVM_DEBUG(dbgs() << nextMunit << ",");
			}
			if (currPath[i].compare(varOwner[varName]) == 0)
			{
				LLVM_DEBUG(dbgs() << "foundi=" << i << "," << nextMunit << ",");
				found = true;
				foundi = i;
				transbasedScalarTransfers[currPath[i]][currMunit].insert(varName);
				connectingPath.push_back(nextMunit);
			}
		}

		assert(std::find(currPath.begin(), currPath.end(), currEdge.first) != currPath.end());

		if (found)
		{
			LLVM_DEBUG(dbgs() << "NewOwner =" << currMunit << "\n");
			varOwner[varName] = currMunit;
			transbasedNextMunit[currPath[foundi]][currMunit].insert(connectingPath);
		}
		else
		{
			LLVM_DEBUG(dbgs() << "\n");
		}
	}

	visitedEdges.insert(currEdge);

	if (currMunit.compare("FUNC_BODY") == 0)
		return;
	for (std::string nextMunit : munitTrans[currMunit])
	{
		assert(nextMunit.compare(currMunit) != 0);
		std::pair<std::string, std::string> nextEdge = std::make_pair(currMunit, nextMunit);
		currPath.push_back(currMunit);
		dfsmunitTrans(nextEdge, currPath, transbasedScalarTransfers, transbasedNextMunit, varOwner, visitedEdges, munitTrans, varNeeds, tabs + 1);
	}
	return;
}

void printFileOutMappingUnitVars(Function &F,
		std::map<std::string, int> *sizeArrMap,
		std::map<Loop *, std::string> loopNames)
{

	std::ofstream outVarMapFile;
	std::string fileName = F.getName().str() + ".outMUVar.csv";
	outVarMapFile.open(fileName.c_str());

	std::map<std::string, std::set<std::string>> munitTrans;
	for (munitTransition mut : munitTransitionsALL)
	{
		std::string srcBBStr = "FUNC_BODY";
		std::string destBBStr = "FUNC_BODY";

		if (BB2MUnitMap.find(mut.srcBB) != BB2MUnitMap.end())
		{
			srcBBStr = BB2MUnitMap[mut.srcBB];
		}
		if (BB2MUnitMap.find(mut.destBB) != BB2MUnitMap.end())
		{
			destBBStr = BB2MUnitMap[mut.destBB];
		}
		munitTrans[srcBBStr].insert(destBBStr);
	}

	std::set<BasicBlock *> restBasicBlocksFn;
	for (BasicBlock &BB : F)
	{
		BasicBlock *BBPtr = cast<BasicBlock>(&BB);
		restBasicBlocksFn.insert(BBPtr);
	}

	//find basic blocks that does not belong to any mapping unit
	for (std::pair<std::string, MappingUnit> pair1 : mappingUnitMap)
	{
		for (BasicBlock *currBB : pair1.second.allBlocks)
		{
			restBasicBlocksFn.erase(currBB);
		}
	}

	std::map<std::string, std::map<std::string, int>> graphMappingUnits;
	std::map<std::string, std::map<std::string, std::set<std::string>>> graphMappingUnitVarNames;

	for (std::pair<std::string, MappingUnit> pair1 : mappingUnitMap)
	{
		for (std::pair<std::string, MappingUnit> pair2 : mappingUnitMap)
		{
			if (pair2.first.compare(pair1.first) == 0)
			{
				continue; // skip children in the same mapping unit
			}
			graphMappingUnits[pair1.first][pair2.first] = 0;
		}
		graphMappingUnits[pair1.first]["FUNC_BODY"] = 0;
		graphMappingUnits["FUNC_BODY"][pair1.first] = 0;
	}

	for (std::pair<std::string, MappingUnit> pair1 : mappingUnitMap)
	{
		for (BasicBlock *currBB : pair1.second.allBlocks)
		{

			for (Instruction &I : *currBB)
			{
				for (User *U : I.users())
				{
					if (Instruction *child = dyn_cast<Instruction>(U))
					{
						BasicBlock *childBB = child->getParent();

						for (std::pair<std::string, MappingUnit> pair2 : mappingUnitMap)
						{
							if (pair2.first.compare(pair1.first) == 0)
							{
								continue; // skip children in the same mapping unit
							}
							if (pair2.second.allBlocks.find(childBB) != pair2.second.allBlocks.end())
							{
								graphMappingUnitVarNames[pair1.first][pair2.first].insert(std::to_string((long)&I));
							}
						}

						//children that go outside of mapping units to function body
						if (restBasicBlocksFn.find(childBB) != restBasicBlocksFn.end())
						{
							graphMappingUnitVarNames[pair1.first]["FUNC_BODY"].insert(std::to_string((long)&I));
						}
					}
				}
			}
		}
	}

	//children of FUNC_BODY
	for (BasicBlock *funcbodyBB : restBasicBlocksFn)
	{
		for (Instruction &I : *funcbodyBB)
		{
			for (User *U : I.users())
			{
				if (Instruction *child = dyn_cast<Instruction>(U))
				{
					BasicBlock *childBB = child->getParent();

					for (std::pair<std::string, MappingUnit> pair2 : mappingUnitMap)
					{
						if (pair2.second.allBlocks.find(childBB) != pair2.second.allBlocks.end())
						{
							graphMappingUnitVarNames["FUNC_BODY"][pair2.first].insert(std::to_string((long)&I));
						}
					}
				}
			}
		}
	}

	//printing the file
	for (std::pair<std::string, std::set<std::string>> pair1 : munitTrans)
	{
		outVarMapFile << pair1.first << ",(";
		for (std::string destMunit : pair1.second)
		{
			outVarMapFile << destMunit << ",";
		}
		outVarMapFile << ")\n";
	}

	for (std::pair<std::string, std::map<std::string, int>> pair1 : graphMappingUnits)
	{
		for (std::pair<std::string, int> pair2 : pair1.second)
		{
			int varSize = graphMappingUnitVarNames[pair1.first][pair2.first].size();
			if (varSize != 0 && mappingUnitMap[pair1.first].lp != mappingUnitMap[pair2.first].lp)
			{

				std::string loop1Name = "FUNC_BODY";
				if (mappingUnitMap[pair1.first].lp != NULL)
					loop1Name = loopNames[mappingUnitMap[pair1.first].lp];

				std::string loop2Name = "FUNC_BODY";
				if (mappingUnitMap[pair2.first].lp != NULL)
					loop2Name = loopNames[mappingUnitMap[pair2.first].lp];

				outVarMapFile << pair1.first << "," << loop1Name << ",";
				outVarMapFile << pair2.first << "," << loop2Name << ",";
				outVarMapFile << varSize << ",";
				for (std::string varName : graphMappingUnitVarNames[pair1.first][pair2.first])
				{
					outVarMapFile << varName << ",";
				}
				outVarMapFile << "\n";
			}
		}
	}

	BasicBlock *startBB = cast<BasicBlock>(&F.getEntryBlock());
	BasicBlock *endBB;

	bool rtiFound = false;
	for (BasicBlock &BB : F)
	{
		for (Instruction &I : BB)
		{
			if (ReturnInst *RTI = dyn_cast<ReturnInst>(&I))
			{
				endBB = RTI->getParent();
				rtiFound = true;
				break;
			}
		}

		if (rtiFound)
		{
			break;
		}
	}

	//Printing array variables
	std::map<std::string, std::map<std::string, int>> grapharrMappingUnits;
	std::map<std::string, std::map<std::string, std::set<std::string>>> grapharrUnitVarNames;

	for (std::pair<std::string, MappingUnit> pair1 : mappingUnitMap)
	{
		for (std::pair<std::string, MappingUnit> pair2 : mappingUnitMap)
		{
			if (pair2.first.compare(pair1.first) == 0)
			{
				continue; // skip children in the same mapping unit
			}
			grapharrMappingUnits[pair1.first][pair2.first] = 0;
		}
		grapharrMappingUnits[pair1.first]["FUNC_BODY"] = 0;
		grapharrMappingUnits["FUNC_BODY"][pair1.first] = 0;
	}

	std::map<std::string, std::set<std::string>> GEPloadMUMap;
	std::map<std::string, std::set<std::string>> GEPstoreMUMap;
	std::map<std::string, Type *> GEPTypeMap;

	for (BasicBlock &BB : F)
	{
		for (Instruction &I : BB)
		{
			if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I))
			{
				LLVM_DEBUG(dbgs() << "Fn : " << F.getName() << "\n");
				LLVM_DEBUG(GEP->dump());

				std::string ptrName = GEP->getPointerOperand()->getName().str();
				LLVM_DEBUG(dbgs() << "PtrName = " << ptrName << "\n");

				Instruction *pointerOp;
				if (ptrName.empty())
				{
					pointerOp = cast<Instruction>(GEP->getPointerOperand());
					LLVM_DEBUG(pointerOp->dump());
					if (LoadInst *pointerOpLD = dyn_cast<LoadInst>(GEP->getPointerOperand()))
					{
						ptrName = pointerOpLD->getPointerOperand()->getName().str();
						LLVM_DEBUG(dbgs() << "NewPtrName = " << ptrName << "\n");
					}
				}
				while (ptrName.empty())
				{
					pointerOp = cast<Instruction>(pointerOp->getOperand(0));
					LLVM_DEBUG(pointerOp->dump());
					if (GetElementPtrInst *OrignalGEP = dyn_cast<GetElementPtrInst>(pointerOp))
					{
						ptrName = OrignalGEP->getPointerOperand()->getName().str();
						LLVM_DEBUG(dbgs() << "NewPtrName = " << ptrName << "\n");
					}
					if (CallInst *CLI = dyn_cast<CallInst>(pointerOp))
					{ //2019 work
						ptrName = CLI->getName().str();
						LLVM_DEBUG(dbgs() << "NewPtrName = " << ptrName << "\n");
					}
				}

				GEPTypeMap[ptrName] = GEP->getSourceElementType();
				GEPstoreMUMap[ptrName].insert(BB2MUnitMap[startBB]);
				for (User *U : GEP->users())
				{
					if (StoreInst *STI = dyn_cast<StoreInst>(U))
					{
						GEPstoreMUMap[ptrName].insert(findMUofBB(STI->getParent()));
						GEPloadMUMap[ptrName].insert(BB2MUnitMap[endBB]);
					}
					if (LoadInst *LDI = dyn_cast<LoadInst>(U))
					{
						GEPloadMUMap[ptrName].insert(findMUofBB(LDI->getParent()));
					}
				}
			}
		}
	}

	std::set<std::pair<Instruction *, std::string>> usesQ;
	for (std::pair<std::string, int> pair1 : *sizeArrMap)
	{
		LLVM_DEBUG(dbgs() << "the user given pointer name = " << pair1.first << "\n");
		for (BasicBlock &BB : F)
		{
			for (Instruction &I : BB)
			{
				if (StoreInst *STI = dyn_cast<StoreInst>(&I))
					continue;
				if (dyn_cast<LoadInst>(&I) && (!dyn_cast<LoadInst>(&I)->getType()->isPointerTy()))
					continue;

				for (int i = 0; i < I.getNumOperands(); ++i)
				{
					if (I.getOperand(i)->getName().str().compare(pair1.first) == 0)
					{
						LLVM_DEBUG(I.dump());
						if (Instruction *childIns = dyn_cast<Instruction>(&I))
						{
							usesQ.insert(std::make_pair(childIns, pair1.first));
						}
					}
				}
			}
		}
	}

	LLVM_DEBUG(dbgs() << "Allocas ::\n");
	for (BasicBlock &BB : F)
	{
		const DataLayout DL = F.getParent()->getDataLayout();
		for (Instruction &I : BB)
		{
			if (AllocaInst *ALI = dyn_cast<AllocaInst>(&I))
			{
				LLVM_DEBUG(ALI->dump());
				if (ALI->isStaticAlloca())
				{
					LLVM_DEBUG(dbgs() << "Size=" << DL.getTypeAllocSize(ALI->getAllocatedType()) << "\n");
					(*sizeArrMap)[ALI->getName().str()] = (int)DL.getTypeAllocSize(ALI->getAllocatedType());
					usesQ.insert(std::make_pair(&I, ALI->getName().str()));
				}
				else
				{
					DFG_CHECK(false, "ParseSizeAttr",
						"non-static alloca found for '" + ALI->getName().str() +
						"'; only static allocations are supported");
				}
			}
		}
	}

	std::map<Value *, std::string> truePointers;
	std::map<std::string, std::string> truePointersStr;

	LLVM_DEBUG(dbgs() << "Collecting True Pointers...\n");
	while (!usesQ.empty())
	{
		std::pair<Instruction *, std::string> head = *usesQ.begin();
		Instruction *I = head.first;
		LLVM_DEBUG(I->dump());
		std::string ptrName = head.second;
		usesQ.erase(head);
		truePointers[I] = ptrName;
		truePointersStr[I->getName().str()] = ptrName;

		for (User *U : I->users())
		{
			if (StoreInst *STI = dyn_cast<StoreInst>(U))
				continue;
			if (dyn_cast<LoadInst>(U) && (!dyn_cast<LoadInst>(U)->getType()->isPointerTy()))
				continue;
			if (Instruction *childIns = dyn_cast<Instruction>(U))
			{
				LLVM_DEBUG(dbgs() << "\tchild=");
				LLVM_DEBUG(childIns->dump());
				if (truePointers.find(childIns) == truePointers.end())
				{
					usesQ.insert(std::make_pair(childIns, ptrName));
				}
				else
				{
					LLVM_DEBUG(dbgs() << "\t\tnot inserted\n");
				}
			}
		}
	}

	LLVM_DEBUG(dbgs() << "TRUE_POINTERS :: \n");
	for (std::pair<Value *, std::string> tppair : truePointers)
	{
		LLVM_DEBUG(dbgs() << tppair.second << "<--");
		LLVM_DEBUG(tppair.first->dump());
	}

	std::map<std::string, int> arrSizes;

	for (std::pair<std::string, std::set<std::string>> pair1 : GEPstoreMUMap)
	{
		std::string ptrName = pair1.first;
		const DataLayout DL = F.getParent()->getDataLayout();
		Type *T = GEPTypeMap[ptrName];
		int size;

		LLVM_DEBUG(T->dump());
		//Determing array size
		if (StructType *ST = dyn_cast<StructType>(T))
		{
			size = DL.getTypeAllocSize(ST);
		}
		else if (ArrayType *AT = dyn_cast<ArrayType>(T))
		{
			size = DL.getTypeAllocSize(AT);
		}
		else
		{
			//TODO: DAC18
			//						 IntegerType* IT = cast<IntegerType>(T);

			if (sizeArrMap->find(ptrName) != sizeArrMap->end())
			{
				size = (*sizeArrMap)[ptrName];
			}
			else if (sizeArrMap->find(truePointersStr[ptrName]) != sizeArrMap->end())
			{
				size = (*sizeArrMap)[ptrName];
			}
			else
			{
				DFG_CHECK(false, "ParseSizeAttr",
					"unknown array size for pointer '" + ptrName +
					"'; please provide sizes using the size annotation attribute");
			}
		}

		arrSizes[ptrName] = size;

		for (std::string StoreMUName : pair1.second)
		{
			for (std::string LoadMUName : GEPloadMUMap[ptrName])
			{
				if (StoreMUName.compare(LoadMUName) == 0)
					continue;
				grapharrMappingUnits[StoreMUName][LoadMUName] += size;
				grapharrUnitVarNames[StoreMUName][LoadMUName].insert(ptrName);
			}
		}
	}

	outVarMapFile << "Arrays Elements : \n";
	for (std::pair<std::string, std::map<std::string, int>> pair1 : grapharrMappingUnits)
	{
		for (std::pair<std::string, int> pair2 : pair1.second)
		{
			if (pair2.second != 0)
			{
				std::string loop1Name = "FUNC_BODY";
				if (mappingUnitMap[pair1.first].lp != NULL)
					loop1Name = loopNames[mappingUnitMap[pair1.first].lp];

				std::string loop2Name = "FUNC_BODY";
				if (mappingUnitMap[pair2.first].lp != NULL)
					loop2Name = loopNames[mappingUnitMap[pair2.first].lp];

				outVarMapFile << pair1.first << "," << loop1Name << ",";
				outVarMapFile << pair2.first << "," << loop2Name << ",";

				outVarMapFile << pair2.second << ",";

				for (std::string ptrName : grapharrUnitVarNames[pair1.first][pair2.first])
				{
					outVarMapFile << ptrName << ",";
				}
				outVarMapFile << "\n";
			}
		}
	}

	outVarMapFile << "*********SMART_TRANSFERS*********\n";

	for (std::pair<std::string, int> pair : arrSizes)
	{
		outVarMapFile << pair.first << "=" << pair.second << "\n";
	}

	outVarMapFile.close();
}

void populateBBTrans()
{
	int id = 0;
	for (std::pair<std::string, MappingUnit> pair : mappingUnitMap)
	{
		for (std::pair<BasicBlock *, BasicBlock *> entryBBPair : pair.second.entryBlocks)
		{
			munitTransition munittrans;
			munittrans.srcBB = entryBBPair.first;
			munittrans.destBB = entryBBPair.second;
			munittrans.id = munitTransitions.size();
			munitTransitions.push_back(munittrans);
			munitTransitionsALL.push_back(munittrans);
		}
	}

	LLVM_DEBUG(dbgs() << "MUNIT transitions = " << munitTransitions.size() << "\n");

	for (std::pair<std::string, MappingUnit> pair : mappingUnitMap)
	{
		for (std::pair<BasicBlock *, BasicBlock *> exitBBPair : pair.second.exitBlocks)
		{
			munitTransition munittrans;
			munittrans.srcBB = exitBBPair.first;
			munittrans.destBB = exitBBPair.second;
			munittrans.id = munitTransitionsALL.size();
			munitTransitionsALL.push_back(munittrans);
		}
	}
}

int getOptimumDim(unsigned int *dimX, unsigned int *dimY, int numNodes, int numMemNodes)
{
	int x;
	int y;
	int numMemPEs;

	int bestWastage = 1000000;
	for (x = 1; x <= 4; ++x)
	{
		for (y = 1; y <= 4; ++y)
		{
			numMemPEs = y;
			int memII = (numMemNodes + numMemPEs) / numMemPEs;
			int resII = (numNodes + x * y) / (x * y);
			int II = std::max(memII, resII);
			int wastage = II * y * x - numNodes;

			if (wastage < bestWastage)
			{
				*dimX = x;
				*dimY = y;
				bestWastage = wastage;
			}
		}
	}
	return bestWastage;
}

/// Recursively collect innermost (leaf) loops into 'result'.
static void collectInnerLoops(Loop *L, SmallVectorImpl<Loop *> &result) {
	if (L->getSubLoops().empty())
		result.push_back(L);
	else
		for (Loop *sub : L->getSubLoops())
			collectInnerLoops(sub, result);
}

/// Hoist loop-invariant double-pointer row dereferences to the loop preheader.
///
/// Targets non-volatile pointer loads of the form:
///
///   (loop body)
///     %base    = load i32**, <global>           -- may be an in-loop load
///     %row_gep = gep i32*, i32** %base, <idx>  -- idx is loop-invariant (NOT IV)
///     %row     = load i32*, i32** %row_gep      -- same value every iteration!
///
/// Because the row index is loop-invariant, the row pointer %row is identical
/// on every iteration.  We move the load to the preheader, eliminating the
/// in-loop memory load without introducing any loop-carried PHI.
///
/// Validity condition (type-based alias check): no store in the loop writes a
/// pointer-typed value, so no store can modify the i32** row-pointer array.
/// In tensor/linear-algebra kernels all in-loop stores write scalars -- the
/// check always passes and hoisting is always safe.
///
/// Two sub-cases handled:
///   arr[fixed_row][IV_col]    -- hoist row_load; body: gep(row, IV) stride-1
///   arr[fixed_row][fixed_col] -- hoist row_load + elem_gep; body: pure OLOAD
///
/// Runs BEFORE FlattenDoublePointers (which handles arr[IV_row][fixed_col]).
void HoistInvariantRowPointers(Function &F, LoopInfo &LI) {
	SmallVector<Loop *, 8> innerLoops;
	for (Loop *L : LI)
		collectInnerLoops(L, innerLoops);

	for (Loop *L : innerLoops) {
		BasicBlock *preheader = L->getLoopPreheader();
		if (!preheader) continue;

		// Find the loop IV so we can skip arr[IV][*] patterns -- those are
		// FlattenDoublePointers' responsibility.
		PHINode *IV = nullptr;
		for (auto &I : *L->getHeader()) {
			PHINode *phi = dyn_cast<PHINode>(&I);
			if (!phi) break;
			if (!phi->getType()->isIntegerTy()) continue;
			if (phi->getNumIncomingValues() != 2) continue;
			unsigned inside = 0;
			for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
				if (L->contains(phi->getIncomingBlock(i))) inside++;
			if (inside == 1) { IV = phi; break; }
		}

		// ------------------------------------------------------------------
		// Safety check: if any store in the loop writes a pointer-typed value
		// we conservatively skip.  A store of i32* or i32** could potentially
		// update the row-pointer array, invalidating the hoist.
		// In all tensor/linear-algebra code, in-loop stores write scalars
		// (e.g. store volatile i32), so this check always passes there.
		// ------------------------------------------------------------------
		bool hasPtrStore = false;
		for (BasicBlock *BB : L->getBlocks()) {
			for (Instruction &I : *BB)
				if (auto *SI = dyn_cast<StoreInst>(&I))
					if (SI->getValueOperand()->getType()->isPointerTy())
						{ hasPtrStore = true; break; }
			if (hasPtrStore) break;
		}
		if (hasPtrStore) continue;

		// ------------------------------------------------------------------
		// Collect hoist candidates: non-volatile pointer loads whose GEP has
		// a single loop-invariant index that is NOT the IV.
		// ------------------------------------------------------------------
		struct HoistPattern {
			LoadInst          *row_load;
			GetElementPtrInst *row_gep;
			Value             *gep_base;
		};
		SmallVector<HoistPattern, 4> patterns;

		for (BasicBlock *BB : L->getBlocks()) {
			for (Instruction &I : *BB) {
				auto *row_load = dyn_cast<LoadInst>(&I);
				if (!row_load || row_load->isVolatile()) continue;
				if (!row_load->getType()->isPointerTy()) continue;

				auto *row_gep = dyn_cast<GetElementPtrInst>(
					row_load->getPointerOperand());
				if (!row_gep || row_gep->getNumIndices() != 1) continue;

				Value *row_idx = *row_gep->idx_begin();
				// arr[IV][*]: FlattenDoublePointers handles this -- skip.
				if (IV && row_idx == IV) continue;
				if (!L->isLoopInvariant(row_idx)) continue;

				// Base must be a pointer-to-pointer (i32**).
				Value *gep_base = row_gep->getPointerOperand();
				auto *ptrTy = dyn_cast<PointerType>(gep_base->getType());
				if (!ptrTy || !ptrTy->getElementType()->isPointerTy()) continue;

				patterns.push_back({row_load, row_gep, gep_base});
			}
		}
		if (patterns.empty()) continue;

		IRBuilder<> preBuilder(preheader->getTerminator());

		for (auto &pat : patterns) {
			// ----------------------------------------------------------------
			// Preheader: recreate the base load if LICM left it in-loop,
			// then hoist the row GEP and row pointer load.
			// ----------------------------------------------------------------
			Value *preheader_base = nullptr;
			if (auto *inLoopLI = dyn_cast<LoadInst>(pat.gep_base)) {
				if (L->contains(inLoopLI->getParent()))
					preheader_base = preBuilder.CreateLoad(
						inLoopLI->getPointerOperand(), "hoist_base");
			}
			if (!preheader_base) {
				if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
					if (L->contains(baseInst->getParent())) continue;
				preheader_base = pat.gep_base;
			}

			Value *row_idx     = *pat.row_gep->idx_begin();
			Value *hoist_addr  = preBuilder.CreateGEP(
				preheader_base, row_idx, "hoist_row_addr");
			Value *hoisted_row = preBuilder.CreateLoad(hoist_addr, "hoist_row");

			// Replace every in-loop use of row_load with the hoisted pointer.
			pat.row_load->replaceAllUsesWith(hoisted_row);
			pat.row_load->eraseFromParent();
			if (pat.row_gep->use_empty())
				pat.row_gep->eraseFromParent();
			if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
				if (baseInst->use_empty())
					baseInst->eraseFromParent();

			// ----------------------------------------------------------------
			// Second-level hoist: any elem GEP that uses hoisted_row and has
			// all loop-invariant indices can itself be moved to the preheader.
			// This covers arr[fixed_row][fixed_col] -- the element pointer is
			// constant across the entire loop, becoming a pure OLOAD in the DFG.
			// ----------------------------------------------------------------
			SmallVector<GetElementPtrInst *, 4> elemGEPs;
			for (User *U : hoisted_row->users()) {
				auto *gep = dyn_cast<GetElementPtrInst>(U);
				if (!gep || !L->contains(gep->getParent())) continue;
				bool allInvariant = true;
				for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it)
					if (!L->isLoopInvariant(*it)) { allInvariant = false; break; }
				if (allInvariant)
					elemGEPs.push_back(gep);
			}
			for (auto *elemGEP : elemGEPs) {
				SmallVector<Value *, 2> idxs(
					elemGEP->idx_begin(), elemGEP->idx_end());
				Value *hoisted_elem = preBuilder.CreateGEP(
					hoisted_row, idxs, "hoist_elem_addr");
				elemGEP->replaceAllUsesWith(hoisted_elem);
				elemGEP->eraseFromParent();
			}
		}
	}
}

/// Fold volatile int** double-pointer 2D array accesses into an element-pointer
/// induction variable, eliminating both the in-loop row-pointer load and the
/// in-loop element GEP node.
///
/// For each innermost loop, we detect the pattern where a row-pointer is
/// loaded inside the loop body using the loop IV as the GEP index:
///
///   (loop body)
///     %base     = load i32**, <global>           -- may be in-loop load
///     %row_gep  = gep i32*, i32** %base, <IV>   -- indexed by loop IV
///     %row      = load i32*, i32** %row_gep      -- in-loop memory load!
///     %elem_gep = gep i32,   i32*  %row, <col>  -- in-loop element GEP!
///
/// And replace with an element-pointer induction variable (no MUL, no in-loop GEP):
///
///   (preheader)
///     %base_pre   = load i32**, <global>
///     %row_init   = load i32*, gep(%base_pre, IV_start)  -- B[k_start]
///     %elem_init  = gep i32, i32* %row_init, <col>       -- B[k_start][j]
///   (header phi)
///     %B_elem = phi [%elem_init, preheader], [%B_elem_next, latch]
///   (loop body)
///     -- B_elem used directly as the element pointer (load B_elem)
///   (latch, before branch)
///     %B_elem_next = gep i32, i32* %B_elem, <N>          -- advance to B[k+1][j]
///
/// By folding the column index into the initial pointer in the preheader,
/// the in-loop element GEP (LS node on CGRA) is eliminated entirely.
/// This matches the DFG node count of an equivalent flat int* access pattern.
void FlattenDoublePointers(Function &F, LoopInfo &LI) {
	SmallVector<Loop *, 8> innerLoops;
	for (Loop *L : LI)
		collectInnerLoops(L, innerLoops);

	for (Loop *L : innerLoops) {
		BasicBlock *preheader = L->getLoopPreheader();
		BasicBlock *latch     = L->getLoopLatch();
		if (!preheader || !latch) continue;

		// ------------------------------------------------------------------
		// 1. Find the loop induction variable: the integer PHI in the header
		//    with one incoming value from outside the loop.
		// ------------------------------------------------------------------
		PHINode *IV        = nullptr;
		Value   *IV_start  = nullptr;   // initial value of IV (from preheader)
		for (auto &I : *L->getHeader()) {
			PHINode *phi = dyn_cast<PHINode>(&I);
			if (!phi) break;
			if (!phi->getType()->isIntegerTy()) continue;
			if (phi->getNumIncomingValues() != 2) continue;
			unsigned inside = 0;
			for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
				if (L->contains(phi->getIncomingBlock(i))) inside++;
			if (inside == 1) {
				IV = phi;
				// Identify which incoming value is from outside the loop
				for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
					if (!L->contains(phi->getIncomingBlock(i)))
						IV_start = phi->getIncomingValue(i);
				break;
			}
		}
		if (!IV || !IV_start) continue;

		// ------------------------------------------------------------------
		// 2. Find N_val: the row stride (number of i32 elements per row).
		//    For polybench: the loop runs k in [k_start, n-1), so n equals
		//    the loop upper bound.  If bound = add(n, -1) then N_val = n.
		// ------------------------------------------------------------------
		Value *N_val = nullptr;
		{
			auto *br = dyn_cast<BranchInst>(latch->getTerminator());
			if (!br || !br->isConditional()) continue;
			auto *cmp = dyn_cast<ICmpInst>(br->getCondition());
			if (!cmp) continue;
			for (unsigned i = 0; i < cmp->getNumOperands() && !N_val; i++) {
				Value *op = cmp->getOperand(i);
				if (!L->isLoopInvariant(op)) continue;
				// Check if op = add(n, -1) → N_val = n
				if (auto *addI = dyn_cast<BinaryOperator>(op)) {
					if (addI->getOpcode() == Instruction::Add) {
						for (unsigned j = 0; j < 2; j++) {
							auto *C = dyn_cast<ConstantInt>(addI->getOperand(j));
							if (C && C->getSExtValue() == -1) {
								N_val = addI->getOperand(1 - j);
								break;
							}
						}
					}
				}
				if (!N_val) N_val = op; // fallback: use bound directly
			}
		}
		if (!N_val) continue;

		// ------------------------------------------------------------------
		// 3. Scan loop blocks for the row-pointer load pattern:
		//      row_load = load i32*, gep(base_ptr, IV)
		//    with exactly one downstream user that is a GEP (elem_gep).
		// ------------------------------------------------------------------
		struct Pattern {
			GetElementPtrInst *row_gep;
			LoadInst          *row_load;
			Value             *gep_base;  // i32** base pointer (may be in-loop)
			GetElementPtrInst *elem_gep;  // gep i32, i32* row_load, col
		};
		SmallVector<Pattern, 4> patterns;

		for (BasicBlock *BB : L->getBlocks()) {
			for (Instruction &I : *BB) {
				auto *row_load = dyn_cast<LoadInst>(&I);
				if (!row_load || row_load->isVolatile()) continue;
				if (!row_load->getType()->isPointerTy()) continue;

				auto *row_gep =
					dyn_cast<GetElementPtrInst>(row_load->getPointerOperand());
				if (!row_gep || row_gep->getNumIndices() != 1) continue;
				if (*row_gep->idx_begin() != IV) continue;

				// GEP base must be pointer-to-pointer (volatile int**)
				Value *gep_base = row_gep->getPointerOperand();
				auto *ptrTy = dyn_cast<PointerType>(gep_base->getType());
				if (!ptrTy || !ptrTy->getElementType()->isPointerTy()) continue;

				// row_load must have exactly one user that is a GEP
				if (!row_load->hasOneUse()) continue;
				auto *elem_gep =
					dyn_cast<GetElementPtrInst>(*row_load->user_begin());
				if (!elem_gep || elem_gep->getNumIndices() != 1) continue;

				patterns.push_back({row_gep, row_load, gep_base, elem_gep});
			}
		}
		if (patterns.empty()) continue;

		LLVMContext    &Ctx        = F.getContext();
		Type           *i32Ty      = Type::getInt32Ty(Ctx);
		IRBuilder<>     preBuilder(preheader->getTerminator());
		IRBuilder<>     latchBuilder(latch->getTerminator());

		for (auto &pat : patterns) {
			// ----------------------------------------------------------------
			// 4a. col_idx must be loop-invariant to fold into the preheader.
			//     Skip this pattern if the column index is loop-variant.
			// ----------------------------------------------------------------
			Value *col_idx = *pat.elem_gep->idx_begin();
			if (!L->isLoopInvariant(col_idx)) continue;

			// ----------------------------------------------------------------
			// 4b. Preheader: load B[k_start] then fold col_idx into it to get
			//     the element pointer for the first iteration B[k_start][j].
			//     If gep_base (the i32** pointer) is an in-loop load (because
			//     LICM did not hoist it), re-create that load in the preheader.
			// ----------------------------------------------------------------
			Value *preheader_base = nullptr;
			if (auto *inLoopLI = dyn_cast<LoadInst>(pat.gep_base)) {
				if (L->contains(inLoopLI->getParent()))
					preheader_base = preBuilder.CreateLoad(
						inLoopLI->getPointerOperand(), "row_outer_ptr");
			}
			if (!preheader_base) {
				if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
					if (L->contains(baseInst->getParent())) continue;
				preheader_base = pat.gep_base;
			}

			// row_init = B[k_start]: row pointer for the first iteration
			Value *Bkinit_addr = preBuilder.CreateGEP(
				preheader_base, IV_start, "row_init_ptr");
			Value *row_init = preBuilder.CreateLoad(Bkinit_addr, "row_init");

			// B_elem_init = B[k_start][j]: fold col_idx into the initial ptr.
			// This eliminates the in-loop element GEP node entirely.
			Value *B_elem_init = preBuilder.CreateGEP(
				row_init, col_idx, "elem_init_ptr");

			// ----------------------------------------------------------------
			// 4c. Create an element-pointer PHI in the loop header.
			//     B_elem tracks B[k][j] directly: no row pointer, no in-loop GEP.
			//     It is initialized to B[k_start][j] and advanced by N elements
			//     per iteration (one full row stride) via a GEP in the latch.
			// ----------------------------------------------------------------
			PHINode *B_elem = PHINode::Create(
				B_elem_init->getType(), 2, "B_elem",
				L->getHeader()->getFirstNonPHI());
			B_elem->addIncoming(B_elem_init, preheader);

			// Latch: B_elem_next = gep i32, i32* B_elem, N_val
			// (advances to the same column in the next row: B[k+1][j])
			Value *N_cast = N_val;
			if (N_val->getType() != i32Ty)
				N_cast = latchBuilder.CreateSExtOrTrunc(N_val, i32Ty, "N_cast");
			Value *B_elem_next = latchBuilder.CreateGEP(B_elem, N_cast, "B_elem_next");
			B_elem->addIncoming(B_elem_next, latch);

			// ----------------------------------------------------------------
			// 4d. Replace elem_gep with B_elem directly -- no new in-loop GEP.
			//     The volatile load of B[k][j] becomes simply: load B_elem.
			// ----------------------------------------------------------------
			pat.elem_gep->replaceAllUsesWith(B_elem);
			pat.elem_gep->eraseFromParent();

			// row_load (now unused) and its GEP can be erased
			pat.row_load->eraseFromParent();
			if (pat.row_gep->use_empty())
				pat.row_gep->eraseFromParent();

			// Erase the in-loop base load if it became dead
			if (auto *baseInst = dyn_cast<Instruction>(pat.gep_base))
				if (baseInst->use_empty())
					baseInst->eraseFromParent();
		}
	}
}

void ParseSizeAttr(Function &F, std::map<std::string, int> *sizeArrMap)
{
	auto global_annos = F.getParent()->getNamedGlobal("llvm.global.annotations");
	if (global_annos)
	{
		auto a = cast<ConstantArray>(global_annos->getOperand(0));
		for (int i = 0; i < a->getNumOperands(); i++)
		{
			auto e = cast<ConstantStruct>(a->getOperand(i));

			if (auto fn = dyn_cast<Function>(e->getOperand(0)->getOperand(0)))
			{
				auto anno = cast<ConstantDataArray>(cast<GlobalVariable>(e->getOperand(1)->getOperand(0))->getOperand(0))->getAsCString();
				fn->addFnAttr("size", anno); // <-- add function annotation here
			}
		}
	}

	if (F.hasFnAttribute("size"))
	{
		Attribute attr = F.getFnAttribute("size");
		LLVM_DEBUG(dbgs() << "Size attribute : " << attr.getValueAsString() << "\n");
		StringRef sizeAttrStr = attr.getValueAsString();
		SmallVector<StringRef, 8> sizeArr;
		sizeAttrStr.split(sizeArr, ',');

		for (int i = 0; i < sizeArr.size(); ++i)
		{
			std::pair<StringRef, StringRef> splitDuple = sizeArr[i].split(':');
			uint32_t size;
			splitDuple.second.getAsInteger(10, size);
			LLVM_DEBUG(dbgs() << "ParseAttr:: name:" << splitDuple.first << ",size:" << size << "\n");
			(*sizeArrMap)[splitDuple.first.str()] = size;
		}
	}

	if (F.hasFnAttribute("size"))
	{
		LLVM_DEBUG(dbgs() << F.getName() << " has my attribute!\n");
	}
}

void RemoveSelectLeafs(Function &F)
{
	for (BasicBlock &BB : F)
	{
		for (Instruction &I : BB)
		{
			Instruction *ins = &I;
			if (SelectInst *SLI = dyn_cast<SelectInst>(ins))
			{
				IRBuilder<> builder(SLI);
				if (Instruction *Cond = dyn_cast<Instruction>(SLI->getCondition()))
				{
					if (Cond->getParent() == SLI->getParent())
						continue;
					LLVM_DEBUG(dbgs() << "Found Alone Select=");
					LLVM_DEBUG(SLI->dump());
					LLVM_DEBUG(dbgs() << "Condition=");
					LLVM_DEBUG(Cond->dump());
					builder.SetInsertPoint(BB.getFirstNonPHI());
					Value *OR0 = builder.CreateXor(Cond, (uint64_t)0);
					assert(OR0 != Cond);
					Instruction *ORIns = cast<Instruction>(OR0);
					LLVM_DEBUG(dbgs() << "OR = ");
					LLVM_DEBUG(ORIns->dump());

					SLI->setOperand(0, OR0);
					LLVM_DEBUG(BB.dump());
				}
			}
		}
	}
}

void InsertORtoSingularConditionalBB(Function &F)
{
	for (BasicBlock &BB : F)
	{
		for (Instruction &I : BB)
		{
			Instruction *ins = &I;
			if (BranchInst *BRI = dyn_cast<BranchInst>(ins))
			{
				if (!BRI->isConditional())
					continue;
				IRBuilder<> builder(BRI);
				if (Instruction *Cond = dyn_cast<Instruction>(BRI->getCondition()))
				{
					if (Cond->getParent() == BRI->getParent())
						continue;
					//Coming here if the condition is from a another basicblock;
					LLVM_DEBUG(dbgs() << "Found Alone Branch=");
					LLVM_DEBUG(BRI->dump());
					LLVM_DEBUG(dbgs() << "Pointer=");
					LLVM_DEBUG(Cond->dump());
					builder.SetInsertPoint(BB.getFirstNonPHI());
					Value *OR0 = builder.CreateXor(Cond, (uint64_t)0);
					assert(OR0 != Cond);
					Instruction *ORIns = cast<Instruction>(OR0);
					LLVM_DEBUG(dbgs() << "OR = ");
					LLVM_DEBUG(ORIns->dump());

					BRI->setOperand(0, OR0);
					LLVM_DEBUG(BB.dump());
				}
			}
		}
	}
	//	    		assert(false);
}

void ReplaceCMPs(Function &F)
{
	dfgNode *node;
	std::vector<Instruction *> instructions;

	for (auto &BB : F)
	{
		for (auto &I : BB)
		{
			instructions.push_back(&I);
		}
	}

	for (auto I : instructions)
	{
		if (CmpInst *CI = dyn_cast_or_null<CmpInst>(I))
		{
			LLVM_DEBUG(I->dump());

			IRBuilder<> builder(CI);
			assert(CI->getNumOperands() == 2);
			switch (CI->getPredicate())
			{
			case CmpInst::ICMP_EQ:
				//TODO : DAC18
			case CmpInst::FCMP_OEQ:
			case CmpInst::FCMP_UEQ:

				break;
			case CmpInst::ICMP_NE:
				//TODO : DAC18
			{
				CmpInst *cmpEqNew = cast<CmpInst>(builder.CreateICmpEQ(CI->getOperand(0), CI->getOperand(1)));
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent()->getInstList(), ii, notIns);
				break;
			}
			case CmpInst::FCMP_ONE:
			case CmpInst::FCMP_UNE:
			{
				CmpInst *cmpEqNew = cast<CmpInst>(builder.CreateFCmpOEQ(CI->getOperand(0), CI->getOperand(1)));
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent()->getInstList(), ii, notIns);
				break;
			}

			case CmpInst::ICMP_SGE:
			case CmpInst::ICMP_UGE:
				//TODO : DAC18
			case CmpInst::FCMP_OGE:
			case CmpInst::FCMP_UGE:
			{
				CmpInst *cmpEqNew;
				if (CI->getPredicate() == CmpInst::ICMP_SGE)
				{
					cmpEqNew = cast<CmpInst>(builder.CreateICmpSLT(CI->getOperand(1), CI->getOperand(0)));
				}
				else
				{ // ICMP_UGE
					cmpEqNew = cast<CmpInst>(builder.CreateICmpULT(CI->getOperand(1), CI->getOperand(0)));
				}
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent()->getInstList(), ii, notIns);
				break;
			}
			case CmpInst::ICMP_SGT:
			case CmpInst::ICMP_UGT:
				//TODO : DAC18
			case CmpInst::FCMP_OGT:
			case CmpInst::FCMP_UGT:

				break;
			case CmpInst::ICMP_SLT:
			case CmpInst::ICMP_ULT:
				//TODO : DAC18
			case CmpInst::FCMP_OLT:
			case CmpInst::FCMP_ULT:

				break;
			case CmpInst::ICMP_SLE:
			case CmpInst::ICMP_ULE:
				//TODO : DAC18
			case CmpInst::FCMP_OLE:
			case CmpInst::FCMP_ULE:
			{
				CmpInst *cmpEqNew;
				if (CI->getPredicate() == CmpInst::ICMP_SLE)
				{
					cmpEqNew = cast<CmpInst>(builder.CreateICmpSGT(CI->getOperand(1), CI->getOperand(0)));
				}
				else
				{ // ICMP_ULE
					cmpEqNew = cast<CmpInst>(builder.CreateICmpUGT(CI->getOperand(1), CI->getOperand(0)));
				}
				Instruction *notIns = cast<Instruction>(builder.CreateNot(cmpEqNew));
				BasicBlock::iterator ii(CI);
				notIns->removeFromParent();
				ReplaceInstWithInst(CI->getParent()->getInstList(), ii, notIns);
				break;
			}
			break;
			default:
				DFG_CHECK(false, "ReplaceCMPs",
					"unsupported comparison predicate (predicate code " +
					std::to_string(CI->getPredicate()) + ")");
				break;
			}
		}
	} //iter thru BB
}

void analyzeAllMappingUnits(Function &F,
		std::map<Loop *, std::string> loopNames)
{

	struct basicblockInfo
	{
		int noNodes;
		std::string mappingunitName;
		std::string loopName;
	};

	std::map<BasicBlock *, basicblockInfo> BasicBlockNN;

	for (std::pair<std::string, MappingUnit> pair : mappingUnitMap)
	{
		std::string munitName = pair.first;
		if (munitName.compare("FUNC_BODY") == 0)
			continue;
		if (pair.second.lp == NULL)
			continue;

		LLVM_DEBUG(dbgs() << "analyzeAllMappingUnits ::" << munitName << "\n");
		std::map<Instruction *, int> insMap;

		DFG LoopDFG("test" + F.getName().str() + "_" + munitName, &loopNames);
		LoopDFG.sizeArrMap = sizeArrMap;
		LoopDFG.setLoopBB(mappingUnitMap[munitName].allBlocks,
				mappingUnitMap[munitName].entryBlocks,
				mappingUnitMap[munitName].exitBlocks);

		insMap.clear();
		for (BasicBlock *B : *LoopDFG.getLoopBB())
		{
			//			  	  BasicBlock *B = *bb;
			int Icount = 0;
			for (auto &I : *B)
			{

				if (insMap.find(&I) != insMap.end())
				{
					continue;
				}

				int depth = 0;
				traverseDefTree(&I, depth, &LoopDFG, &insMap, BBSuccBasicBlocks, *LoopDFG.getLoopBB());
			}
		}
		LoopDFG.addPHIChildEdges();
		LoopDFG.connectBB();
		printDFGDOT(LoopDFG.getName() + "_loopdfg.dot", &LoopDFG);

		LoopDFG.handlePHINodes(mappingUnitMap[munitName].allBlocks);

		LoopDFG.removeRedEdgesPHI();
		LoopDFG.addCMERGEtoSELECT();
		LoopDFG.handlestartstop_munit(munitTransitions);

		LoopDFG.treatFalsePaths();
		LoopDFG.insertshiftGEPs();
		LoopDFG.addMaskLowBitInstructions();
		LoopDFG.checkSanity();
		printDFGDOT(LoopDFG.getName() + "_loopdfg.dot", &LoopDFG);

		LoopDFG.scheduleASAP();
		LoopDFG.scheduleALAP();
		LoopDFG.balanceASAPALAP();
		LoopDFG.CreateSchList();
		LoopDFG.printXML();
		LoopDFG.handleMEMops();
		LoopDFG.partitionMemNodes();

		for (dfgNode *node : LoopDFG.getNodes())
		{

			BasicBlock *currBB = (BasicBlock *)node->BB;
			if (node->getNameType().compare("LOOPEXIT") == 0)
			{
				currBB = (BasicBlock *)node->getAncestors()[0]->BB;
			}

			if (BasicBlockNN.find(currBB) == BasicBlockNN.end())
			{
				basicblockInfo bbInfoIns;
				bbInfoIns.noNodes = 1;
				bbInfoIns.mappingunitName = munitName;
				bbInfoIns.loopName = loopNames[pair.second.lp];
				BasicBlockNN[currBB] = bbInfoIns;

				LLVM_DEBUG(node->printName());
				LLVM_DEBUG(dbgs() << "Adding...\n");
				LLVM_DEBUG(dbgs() << "currBB name = " << currBB->getName().str() << "\n");
				LLVM_DEBUG(dbgs() << " munitName = " << munitName << "\n");
				LLVM_DEBUG(dbgs() << " loopNames[pair.second.lp] = " << loopNames[pair.second.lp] << "\n");
			}
			else
			{
				LLVM_DEBUG(node->printName());
				LLVM_DEBUG(dbgs() << "Incrementing...\n");
				LLVM_DEBUG(dbgs() << "currBB name = " << currBB->getName().str() << "\n");
				LLVM_DEBUG(dbgs() << "BasicBlockNN[currBB].mappingunitName = " << BasicBlockNN[currBB].mappingunitName);
				LLVM_DEBUG(dbgs() << " munitName = " << munitName << "\n");
				LLVM_DEBUG(dbgs() << "BasicBlockNN[currBB].loopName = " << BasicBlockNN[currBB].loopName);
				LLVM_DEBUG(dbgs() << " loopNames[pair.second.lp] = " << loopNames[pair.second.lp] << "\n");
				assert(BasicBlockNN[currBB].mappingunitName.compare(munitName) == 0);
				assert(BasicBlockNN[currBB].loopName.compare(loopNames[pair.second.lp]) == 0);
				BasicBlockNN[currBB].noNodes = BasicBlockNN[currBB].noNodes + 1;
			}
		}
	}

	std::ofstream BasicBlockNNFile;
	BasicBlockNNFile.open("BasicBlockNNFile.log");
	for (std::pair<BasicBlock *, basicblockInfo> pair : BasicBlockNN)
	{
		BasicBlockNNFile << pair.first->getName().str() << ",";
		BasicBlockNNFile << pair.second.noNodes << ",";
		BasicBlockNNFile << pair.second.loopName << ",";
		BasicBlockNNFile << pair.second.mappingunitName << "\n";
	}
	BasicBlockNNFile.close();
}

void loopTrace(std::map<Loop *, std::string> loopNames, Function &F, LoopTree rootLoop)
{

	LLVMContext &Ctx = F.getContext();
	BasicBlock &FentryBB = F.getEntryBlock();
	IRBuilder<> builder(FentryBB.getFirstNonPHI());

	//Function Calls

	auto traceStartFn = F.getParent()->getOrInsertFunction(
			"loopTraceOpen",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx));

	auto traceEndFn = F.getParent()->getOrInsertFunction(
			"loopTraceClose",
			FunctionType::getVoidTy(Ctx));

	auto loopInvFn = F.getParent()->getOrInsertFunction(
			"loopInvoke",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx));

	auto loopInsUpdateFn = F.getParent()->getOrInsertFunction(
			"loopInsUpdate",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt32Ty(Ctx));

	auto loopBBInsUpdateFn = F.getParent()->getOrInsertFunction(
			"loopBBInsUpdate",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt32Ty(Ctx));

	auto loopInsClearFn = F.getParent()->getOrInsertFunction(
			"loopInsClear",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx));

	auto loopBBInsClearFn = F.getParent()->getOrInsertFunction(
			"loopBBInsClear",
			FunctionType::getVoidTy(Ctx));

	auto loopInvokeEndFn = F.getParent()->getOrInsertFunction(
			"loopInvokeEnd",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx));

	auto reportExecInsCountFn = F.getParent()->getOrInsertFunction(
			"reportExecInsCount",
			FunctionType::getVoidTy(Ctx),
			Type::getInt32Ty(Ctx));

	auto updateLoopPreHeaderFn = F.getParent()->getOrInsertFunction(
			"updateLoopPreHeader",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt8PtrTy(Ctx));

	auto loopBBMappingUnitUpdate = F.getParent()->getOrInsertFunction(
			"loopBBMappingUnitUpdate",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt8PtrTy(Ctx));

	auto recordUncondMunitTransition = F.getParent()->getOrInsertFunction(
			"recordUncondMunitTransition",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt8PtrTy(Ctx));

	auto recordCondMunitTransition = F.getParent()->getOrInsertFunction(
			"recordCondMunitTransition",
			FunctionType::getVoidTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt8PtrTy(Ctx),
			Type::getInt1Ty(Ctx));

	for (std::pair<Loop *, std::string> lnPair : loopNames)
	{
		Value *loopName = builder.CreateGlobalStringPtr(lnPair.second);
		Value *loopNameLLVM = builder.CreateGlobalStringPtr(lnPair.second + "-" + lnPair.first->getLoopPreheader()->getName().str());
		BasicBlock *loopHeader = lnPair.first->getLoopPreheader();
		builder.SetInsertPoint(loopHeader, loopHeader->getFirstInsertionPt());
		builder.CreateCall(loopInvFn, {loopName});
		builder.CreateCall(loopInsClearFn, {loopName});

		for (BasicBlock *BB : loopsExclusieBasicBlockMap[lnPair.first])
		{
			int instructionCountBB = BB->getInstList().size();
			Value *instructionCountBBVal = ConstantInt::get(Type::getInt32Ty(Ctx), instructionCountBB);
			Value *BBName = builder.CreateGlobalStringPtr(BB->getName());

			builder.SetInsertPoint(BB, --BB->end());
			builder.CreateCall(loopInsUpdateFn, {loopName, instructionCountBBVal});
			builder.CreateCall(loopBBInsUpdateFn, {loopName, BBName, instructionCountBBVal});
		}

		SmallVector<BasicBlock *, 8> loopExitBlocks;
		lnPair.first->getExitBlocks(loopExitBlocks);
		for (BasicBlock *BB : loopExitBlocks)
		{
			builder.SetInsertPoint(BB, --BB->end());
			builder.CreateCall(loopInvokeEndFn, {loopName});
		}
	}

	//adding top most level loop's preheaders
	for (LoopTree lt : rootLoop.lpChildren)
	{
		BasicBlock *lpPreHeaderBB = lt.lp->getLoopPreheader();
		Value *loopName = builder.CreateGlobalStringPtr(loopNames[lt.lp]);

		int instructionCountBB = lpPreHeaderBB->getInstList().size();
		Value *instructionCountBBVal = ConstantInt::get(Type::getInt32Ty(Ctx), instructionCountBB);
		Value *BBName = builder.CreateGlobalStringPtr(lpPreHeaderBB->getName());

		builder.SetInsertPoint(lpPreHeaderBB, --lpPreHeaderBB->end());
		builder.CreateCall(loopBBInsUpdateFn, {loopName, BBName, instructionCountBBVal});
	}

	//find return instruction
	for (BasicBlock &BB : F)
	{
		Value *insCountBBVal = ConstantInt::get(Type::getInt32Ty(Ctx), BB.getInstList().size());
		builder.SetInsertPoint(&BB, --BB.end());
		builder.CreateCall(reportExecInsCountFn, {insCountBBVal});

		for (Instruction &I : BB)
		{
			if (ReturnInst *RI = dyn_cast<ReturnInst>(&I))
			{
				BasicBlock *retBB = RI->getParent();
				builder.SetInsertPoint(retBB, --retBB->end());
				builder.CreateCall(traceEndFn);
			}
		}
	}

	std::set<BasicBlock *> srcMunitTrans;
	for (munitTransition munitTrans : munitTransitionsALL)
	{
		srcMunitTrans.insert(munitTrans.srcBB);
	}

	for (BasicBlock *srcBB : srcMunitTrans)
	{
		for (Instruction &I : *srcBB)
		{
			Value *srcBBName = builder.CreateGlobalStringPtr(srcBB->getName());
			if (BranchInst *BRI = dyn_cast<BranchInst>(&I))
			{
				if (BRI->isConditional())
				{
					Value *dest1BBName = builder.CreateGlobalStringPtr(BRI->getSuccessor(0)->getName());
					Value *dest2BBName = builder.CreateGlobalStringPtr(BRI->getSuccessor(1)->getName());
					Value *condition = BRI->getCondition();
					builder.SetInsertPoint(BRI);
					builder.CreateCall(recordCondMunitTransition, {srcBBName, dest1BBName, dest2BBName, condition});
				}
				else
				{
					Value *destBBName = builder.CreateGlobalStringPtr(BRI->getSuccessor(0)->getName());
					builder.SetInsertPoint(BRI);
					builder.CreateCall(recordUncondMunitTransition, {srcBBName, destBBName});
				}
			}
		}
	}

	builder.SetInsertPoint(&FentryBB, FentryBB.getFirstInsertionPt());
	Value *fnName = builder.CreateGlobalStringPtr(F.getName().str());
	builder.CreateCall(traceStartFn, {fnName});
	builder.CreateCall(loopBBInsClearFn);

	//adding BB and mapping unit name relationship to the instrumentation lib
	for (std::pair<std::string, MappingUnit> pair : mappingUnitMap)
	{
		for (BasicBlock *BB : pair.second.allBlocks)
		{
			Value *BBName = builder.CreateGlobalStringPtr(BB->getName());
			Value *MunitName = builder.CreateGlobalStringPtr(pair.first);
			builder.CreateCall(loopBBMappingUnitUpdate, {BBName, MunitName});
		}
	}

	//denoting loops preheaders in the instrumentation library
	for (std::pair<Loop *, std::string> lnPair : loopNames)
	{
		Value *loopName = builder.CreateGlobalStringPtr(lnPair.second);
		Value *preHeaderBBName = builder.CreateGlobalStringPtr(lnPair.first->getLoopPreheader()->getName());
		builder.CreateCall(updateLoopPreHeaderFn, {loopName, preHeaderBBName});
	}
}

std::string getMappingUnitNameUsingTokenFunction(Function &F)
{
	BasicBlock *MUBB;
	Instruction *checker_ins = NULL;
	for (auto &BB : F)
	{
		// BB.dump();
		for (auto &I : BB)
		{
			if (CallInst *CI = dyn_cast<CallInst>(&I))
			{
				Function *calledFn = CI->getCalledFunction();
				if (!calledFn) {
					// Handle calls through bitcasts (e.g., from implicit function declarations)
					if (auto *CE = dyn_cast<ConstantExpr>(CI->getCalledOperand()))
						if (CE->getOpcode() == Instruction::BitCast)
							calledFn = dyn_cast<Function>(CE->getOperand(0));
				}
				if (calledFn && calledFn->getName() == "please_map_me")
				{
					LLVM_DEBUG(dbgs()  << "token found in BB = " << BB.getName() << "\n");
					MUBB = &BB;
					checker_ins = CI;
				}
			}
		}
	}
	DFG_CHECK(MUBB && checker_ins,
		"getMappingUnitNameUsingTokenFunction",
		"could not find please_map_me() annotation in function; "
		"ensure the target loop body contains a call to please_map_me()");
	checker_ins->eraseFromParent();

	for (auto it = mappingUnitMap.begin(); it != mappingUnitMap.end(); it++)
	{
		std::set<BasicBlock *> bbs = it->second.allBlocks;
		if (bbs.find(MUBB) != bbs.end())
		{
			return it->first;
		}
	}
	DFG_CHECK(false,
		"getMappingUnitNameUsingTokenFunction",
		"please_map_me() was found but its basic block does not belong to any known mapping unit");
}

void AllocateSPMBanks(std::unordered_set<Value *> &outer_vals,
		std::unordered_map<Value *, GetElementPtrInst *> &mem_ptrs,
		std::unordered_map<Value *, int> &acc,
		std::unordered_map<Value *, SPM_BANK> &spm_bank_allocation,
		std::unordered_map<Value *, int> &spm_base_address,
		Function &F)
{


	LLVM_DEBUG(dbgs()<<"number of banks: "<<banks_number<<"\n");
	LLVM_DEBUG(dbgs()<<"banks size: "<<bank_size<<"\n");
	LLVM_DEBUG(dbgs()<<"Data placement policy: "<<dp_policy<<"\n");
	// Find variable sizes;
	std::unordered_map<Value *, int> variable_sizes_bytes;
	DataLayout DL = F.getParent()->getDataLayout();

	//for outer vals
	LLVM_DEBUG(dbgs()<<"For outer values \n");
	for (auto it = outer_vals.begin(); it != outer_vals.end(); it++)
	{
		Value *outer_val = *it;
		LLVM_DEBUG(outer_val->dump());
		int size = DL.getTypeAllocSize(outer_val->getType());
		LLVM_DEBUG(dbgs() <<" Size:" << size << "/n");
		assert(size <= bank_size); // assume the size of each array is not bigger than the bank size
		variable_sizes_bytes[outer_val] = size;
	}

	// for mem ptrs
	for (auto it = mem_ptrs.begin(); it != mem_ptrs.end(); it++)
	{
		Value *mem_value = it->first;
		GetElementPtrInst *gep = it->second;
		std::string gep_pointer_name = gep->getPointerOperand()->getName();
		if (sizeArrMap.find(gep_pointer_name) != sizeArrMap.end())
		{
			variable_sizes_bytes[gep->getPointerOperand()] = sizeArrMap[gep_pointer_name];
			assert(variable_sizes_bytes[gep->getPointerOperand()] <= bank_size);
			LLVM_DEBUG(dbgs() << gep_pointer_name << ", size = " << sizeArrMap[gep_pointer_name] << "\n");
		}
		else
		{
			int size = DL.getTypeAllocSize(gep->getSourceElementType());
			assert(size <= bank_size);
			variable_sizes_bytes[gep->getPointerOperand()] = size;
			LLVM_DEBUG(dbgs() << gep_pointer_name << ", size = " << size << "\n");
		}
	}



	std::vector<std::unordered_set<Value*>> banks_vars;
	std::map<Value*, int> value_to_BankId;
	std::map<int, int> data_in_bank;
	for(int i = 0; i < banks_number ; i++){
		std::unordered_set<Value*> temp;
		banks_vars.push_back(temp);
		data_in_bank [i] = 0;
	}


	//data placement
	/*assign acc (arrays and scalars) to memories. It balances the data amount (size) of each bank.
	Currently don't consider number of accesses for each array. */
	{

		int desired_bank = 0;
		for(auto it = acc.begin(); it != acc.end(); it++){
			int size = variable_sizes_bytes[it->first];
			int least_data = data_in_bank[0];
			if(dp_policy ==0){ //Data placement policy 0 : balances the data amount (size) of each bank.
				desired_bank = 0;
				for(int i = 0 ; i< banks_number;i++){
					if(data_in_bank [i] < least_data ){
						least_data = data_in_bank [i];
						desired_bank = i;
					}
				}

			}else{ //Data placement policy 1 : place arrays on alternative banks

				if(desired_bank == banks_number-1){
					desired_bank = 0;
				}else{
					desired_bank = desired_bank + 1;
				}
			}

			LLVM_DEBUG(dbgs()<<"assign"<< size << "to bank"<<desired_bank<<"\n");
			banks_vars[desired_bank].insert(it->first);
			value_to_BankId[it->first] = desired_bank;
			data_in_bank[desired_bank] = size + data_in_bank[desired_bank];
			assert(data_in_bank[desired_bank] < bank_size);

		}
	}


	for(int i = 0; i < banks_vars.size(); i++){
		auto & bank_vars = banks_vars[i];
		LLVM_DEBUG(dbgs() << "Bank"<<i<< " vars :: \n");
		for(Value* v : bank_vars){
			LLVM_DEBUG(dbgs() << "\t" << v->getName() << " :: size = " << variable_sizes_bytes[v] << ", acceses = " << acc[v] << "\n");
			// spm_bank_allocation[v]=BANK0;
			// spm_base_address[v] = bank0_addr;
			// bank0_addr += variable_sizes_bytes[v];
		}
	}

	int mem_size = banks_number * bank_size;

	LLVM_DEBUG(dbgs() << "FINAL ALLOCATION BEGIN.\n");
	std::map<int, int> bank_base_address;
	for(int i = 0; i< banks_number;i++){
		bank_base_address.emplace(i, i * bank_size);
	}
	for (auto it = mem_ptrs.begin(); it != mem_ptrs.end(); it++)
	{
		Value* mem_ins = it->first;
		GetElementPtrInst* gep = it->second;

		LLVM_DEBUG(dbgs() << "pointer_ins = " << mem_ins->getName() << ",");
		LLVM_DEBUG(dbgs()<< "gep_pointer = " << gep->getPointerOperand()->getName() << ",");
		LLVM_DEBUG(dbgs() << "size = " << variable_sizes_bytes[gep->getPointerOperand()] << ",");

		if(value_to_BankId.find(gep->getPointerOperand()) != value_to_BankId.end()){
			if(spm_base_address.find(gep->getPointerOperand()) == spm_base_address.end()){
				int bank_id = (value_to_BankId.find(gep->getPointerOperand()))->second;
				spm_bank_allocation[gep->getPointerOperand()] = SPMBANKOfIndex(bank_id);
				LLVM_DEBUG(dbgs() << "bank="<<bank_id<<",");
				spm_base_address[gep->getPointerOperand()] = bank_base_address[bank_id];
				assert(variable_sizes_bytes.find(gep->getPointerOperand()) != variable_sizes_bytes.end());
				bank_base_address[bank_id] += variable_sizes_bytes[gep->getPointerOperand()];
				LLVM_DEBUG(dbgs() << "addr=" << spm_base_address[gep->getPointerOperand()] << "\n");
			}
			else{
				LLVM_DEBUG(dbgs() << "array/struct already allocated \n");
			}
		}
		else{
			DFG_CHECK(false, "AllocateSPMBanks",
				"GEP pointer '" + gep->getPointerOperand()->getName().str() +
				"' has no assigned SPM bank; check data placement configuration");
		}
	}

	for (auto it = outer_vals.begin(); it != outer_vals.end(); it++)
	{
		Value* outer_value_mem = *it;

		LLVM_DEBUG(dbgs() << "outer value = " << outer_value_mem->getName() << ",");
		if(value_to_BankId.find(outer_value_mem) != value_to_BankId.end()){
			int bank_id = (value_to_BankId.find(outer_value_mem))->second;
			spm_bank_allocation[outer_value_mem] = SPMBANKOfIndex(bank_id);
			spm_base_address[outer_value_mem] = bank_base_address[bank_id];
			assert(variable_sizes_bytes.find(outer_value_mem) != variable_sizes_bytes.end());
			bank_base_address[bank_id] += variable_sizes_bytes[outer_value_mem];

			LLVM_DEBUG(dbgs() << "bank="<<bank_id<<",");
			LLVM_DEBUG(dbgs() << "addr=" << spm_base_address[outer_value_mem] << "\n");
		}
		else{
			DFG_CHECK(false, "AllocateSPMBanks",
				"outer value '" + outer_value_mem->getName().str() +
				"' has no assigned SPM bank; check data placement configuration");
		}

	}

	LLVM_DEBUG(dbgs() << "FINAL ALLOCATION END.\n");


	// assert(false);

}
namespace
{

struct dfggenPass : public FunctionPass
{
	static char ID;
	dfggenPass() : FunctionPass(ID)
	{
		//    	initializeSkeletonFunctionPassPass(*PassRegistry::getPassRegistry());
	}

	virtual bool runOnFunction(Function &F)
	{
	  try {
		std::map<Instruction *, int> insMap;
		std::map<Instruction *, int> insMap2;
		static std::set<const BasicBlock *> funcBB;
		std::error_code EC;

		std::ofstream timeFile;
		std::string timeFileName = "time." + F.getName().str() + ".log";
		timeFile.open(timeFileName.c_str());
		clock_t begin = clock();
		clock_t end;
		std::string loopCFGFileName;

		MEM_SIZE = banks_number * bank_size;

		if (fName != "na")
		{
			if (F.getName() != fName)
			{
				LLVM_DEBUG(dbgs() << "Function Name : " << F.getName() << "\n");
				return false;
			}
		}


		ReplaceCMPs(F);
		RemoveSelectLeafs(F);
		InsertORtoSingularConditionalBB(F);
		ParseSizeAttr(F, &sizeArrMap);

		std::string Filename = ("cfg." + F.getName() + ".dot").str();
		//errs() << "Writing '" << Filename << "'...";

		raw_fd_ostream File(Filename, EC, sys::fs::F_Text);

		if (!EC)
		{
			WriteGraph(File, (const Function *)&F);
		}
		else
		{
			errs() << "  error opening file for writing!";
			errs() << "\n";
		}

		LLVM_DEBUG(dbgs() << "Processing : " << F.getName() << "\n");

		LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
		ScalarEvolution *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
		DependenceInfo *DI = &getAnalysis<DependenceAnalysisWrapperPass>().getDI();
		DominatorTree *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
		const DataLayout &DL = F.getParent()->getDataLayout();

		// Pass 1: hoist loop-invariant row-pointer loads (arr[fixed][*]) to
		// the preheader -- no new PHI, type-based alias safety check.
		HoistInvariantRowPointers(F, LI);

		// Pass 2: fold arr[IV][fixed] row-pointer loads into a loop-carried
		// element-pointer PHI, eliminating the in-loop memory load for B[k][j].
		FlattenDoublePointers(F, LI);

		MemDepResult mRes;

		int loopCounter = 0;

		SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 1> BackEdgesBB;
		FindFunctionBackedges(F, BackEdgesBB);

		int BBCount = 0;
		for (auto &B : F)
		{
			BBCount++;
		}

		int currBBIdx = 0;
		for (auto &B : F)
		{
			currBBIdx++;
			LLVM_DEBUG(dbgs() << "Currently processing = " << currBBIdx << "\n");
			BasicBlock *BB = dyn_cast<BasicBlock>(&B);
			funcBB.insert(BB);
			BBSuccBasicBlocks[BB].push_back(BB);
			dfsBB(BackEdgesBB, &BBSuccBasicBlocks, BB, BB);
		}
		printBBSuccMap(F, BBSuccBasicBlocks);

		end = clock();
		timeFile << "Preprocessing Time = " << double(end - begin) / CLOCKS_PER_SEC << "\n";
		insMap.clear();

		std::vector<Loop *> innerMostLoops;
		std::map<Loop *, std::string> loopNames;
		std::map<Loop *, DFG *> loopDFGs;
		std::vector<Loop *> loops;
		for (LoopInfo::iterator i = LI.begin(); i != LI.end(); ++i)
		{
			Loop *L = *i;
			loops.push_back(L);
		}

		populateNonLoopBBs(F, loops);
		std::string lnstr("LN");
		getInnerMostLoops(&innerMostLoops, loops, &loopNames, lnstr, &rootLoop);
		LLVM_DEBUG(dbgs() << "Number of innermost loops : " << innerMostLoops.size() << "\n");

		printLoopTree(rootLoop, &loopNames);
		printMappableUnitMap();
		populateBBTrans();

		std::string munitName = getMappingUnitNameUsingTokenFunction(F);
		// std::string munitName = "INNERMOST_LN121";
		LLVM_DEBUG(dbgs() << "--------------------------------------------------------------\n");
		LLVM_DEBUG(dbgs() << "--------MAPPING-------" << munitName << " of " << F.getName() << "---------------\n");
		LLVM_DEBUG(dbgs() << "--------------------------------------------------------------\n");

		// Validate loop body for unsupported patterns
		for (BasicBlock *BB : mappingUnitMap[munitName].allBlocks) {
			for (Instruction &I : *BB) {
				if (CallInst *CI = dyn_cast<CallInst>(&I)) {
					Function *calledFn = CI->getCalledFunction();
					if (!calledFn) {
						if (auto *CE = dyn_cast<ConstantExpr>(CI->getCalledOperand()))
							if (CE->getOpcode() == Instruction::BitCast)
								calledFn = dyn_cast<Function>(CE->getOperand(0));
					}
					if (calledFn && !calledFn->isIntrinsic() && calledFn->getName() != "please_map_me") {
						errs() << "Warning: function call to '" << calledFn->getName()
							   << "' found in loop body; this may cause issues in DFG generation\n";
					}
				}
			}
		}

		//-----------------------------------
		// New Code for 2018 work
		//-----------------------------------
		{
			DFGPartPred *LoopDFG = new DFGPartPred(F.getName().str() + "_" + munitName, &loopNames, mappingUnitMap[munitName].lp);
			LoopDFG->setKernelName(F.getName().str());
			LoopDFG->SE = SE;
			LoopDFG->DT = DT;

			LoopDFG->setBBSuccBasicBlocks(BBSuccBasicBlocks);
			LoopDFG->sizeArrMap = sizeArrMap;
			LLVM_DEBUG(dbgs() << "Currently mapping unit : " << munitName << "\n");
			assert(!mappingUnitMap[munitName].allBlocks.empty());
			LoopDFG->setLoopBB(mappingUnitMap[munitName].allBlocks,
					mappingUnitMap[munitName].entryBlocks,
					mappingUnitMap[munitName].exitBlocks);
			insMap.clear();
			LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][traverseDefTree begin]\n");
			for (BasicBlock *B : *LoopDFG->getLoopBB())
			{
				int Icount = 0;
				for (auto &I : *B)
				{

					if (insMap.find(&I) != insMap.end())
					{
						continue;
					}

					int depth = 0;
					traverseDefTree(&I, depth, LoopDFG, &insMap, BBSuccBasicBlocks, *LoopDFG->getLoopBB());
				}
			}
			LLVM_DEBUG(dbgs() << "[Skeleton.cpp][traverseDefTree end]\n\n");

			LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][addMemRecDepEdgesNew begin]\n");
			LoopDFG->addMemRecDepEdgesNew(DI);
			LLVM_DEBUG(dbgs() << "[Skeleton.cpp][addMemRecDepEdgesNew end]\n\n");


			LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][generateTrigDFGDOT begin]\n");
			LoopDFG->generateTrigDFGDOT(F);
			LLVM_DEBUG(dbgs() << "[Skeleton.cpp][generateTrigDFGDOT end]\n\n");

			{
				std::unordered_set<Value *> outVals;
				std::unordered_map<Value *, GetElementPtrInst *> arrPtrs;
				std::unordered_map<Value *, int> mem_acceses; // base pointer name : number of memory accesses
				std::map<dfgNode*,Value*> OLNodesWithPtrTyUsage;
				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][getTransferVariables begin]\n");
				LoopDFG->getTransferVariables(outVals, arrPtrs, mem_acceses, F);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][getTransferVariables end]\n\n");
				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][SetBasePointers begin]\n");
				LoopDFG->SetBasePointers(outVals, arrPtrs,OLNodesWithPtrTyUsage, F);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][SetBasePointers end]\n\n");

				LLVM_DEBUG(dbgs()<<"\nOutvals contains: \n" );
				for (auto it = outVals.begin(); it != outVals.end(); it++)
				{
					Value* outvl = *it;
					LLVM_DEBUG(dbgs() << "outVal:");
					LLVM_DEBUG(outvl->dump());
					LLVM_DEBUG(dbgs() << "\n");
				}

				LLVM_DEBUG(dbgs()<<"\nsizeArrMap contains: \n") ;
				for (auto it = sizeArrMap.begin(); it != sizeArrMap.end(); it++)
				{
					std::string base_ptr = it->first;
					int size = it->second;
					LLVM_DEBUG(dbgs() << "base_ptr:" << base_ptr << ", size = " << size << "\n");
				}

				LLVM_DEBUG(dbgs()<<"\narrPtrs contains: \n") ;
				for (auto it = arrPtrs.begin(); it != arrPtrs.end(); it++)
				{
					Value *base_ptr = it->first;
					GetElementPtrInst * gep = it->second;
					LLVM_DEBUG(dbgs() << "base_ptr:" << base_ptr->getName() << ", GEP = ");
					LLVM_DEBUG(gep->dump());
				}
				LLVM_DEBUG(dbgs()<<"\nmem_acceses contains: \n") ;
				for (auto it = mem_acceses.begin(); it != mem_acceses.end(); it++)
				{
					Value *base_ptr = it->first;
					int accesses = it->second;
					LLVM_DEBUG(dbgs() << "base_ptr:" << base_ptr->getName() << ", accesses = " << accesses << "\n");
				}

				//for hycube binary generation
				std::unordered_map<Value *, SPM_BANK> spm_bank_allocation;
				std::unordered_map<Value *, int> spm_base_address;
				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][AllocateSPMBanks] begin\n");
				AllocateSPMBanks(outVals,arrPtrs,mem_acceses,spm_bank_allocation,spm_base_address,F);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][AllocateSPMBanks] end\n\n");

				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][UpdateSPMAllocation] begin\n");
				LoopDFG->UpdateSPMAllocation(spm_base_address,spm_bank_allocation,arrPtrs);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][UpdateSPMAllocation] end\n\n");

				/* UpdateSPMAllocation re-sets GEPbaseAddr (constVal) on all GEP nodes.
				 * For 2-D GEPs whose OLOAD ancestor already carries spm_base+idx*stride,
				 * the ADD must use two register inputs (constValid=false).
				 * Clear GEPbaseAddr for those nodes so the mapper emits constValid=0. */
				LoopDFG->clearGEPbaseAddrForOLOADnodes();
				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][InstrumentInOutVars begin]\n");
				LoopDFG->InstrumentInOutVars(F,mem_acceses,OLNodesWithPtrTyUsage,spm_base_address);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][InstrumentInOutVars end]\n\n");
			}
			//std::cout << "Code instrumentation done \n";

			LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][PrintOuts] begin\n");
			LoopDFG->PrintOuts();
			LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][PrintOuts] end\n");
			delete (LoopDFG);
			return true;
		}
	  } catch (const DFGGenerationError &e) {
		errs() << "Error: " << e.what() << "\n";
		return false;
	  }
	} //END OF runOnFunction

	void getAnalysisUsage(AnalysisUsage &AU) const override
	{
		//			AU.setPreservesAll();
		//			AU.addRequired<LoopInfoWrapperPass>();
		//			AU.addRequired<MemoryDependenceAnalysis>();
		AU.addRequired<DependenceAnalysisWrapperPass>();

		AU.setPreservesAll();
		AU.addRequired<LoopInfoWrapperPass>();
		//			AU.addRequired<MemoryDependenceAnalysis>();
		AU.addRequired<ScalarEvolutionWrapperPass>();
		AU.addRequired<AAResultsWrapperPass>();
		AU.addRequired<DominatorTreeWrapperPass>();
		//		    AU.addRequired<DependenceAnalysis>();
		AU.addRequiredID(LoopSimplifyID);
		AU.addRequiredID(LCSSAID);

		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addPreserved<DominatorTreeWrapperPass>();
	}
};

} // namespace

char dfggenPass::ID = 1;
static RegisterPass<dfggenPass> X("skeleton", "SkeletonFunctionPass", false, false);
