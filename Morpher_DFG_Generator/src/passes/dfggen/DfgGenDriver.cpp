#include "DfgGenInternal.h"

AttributeList attr21;

bool xmlRun = false;

int MEM_SIZE;

using namespace llvm;
//cl::opt<unsigned> loopNumber("ln", cl::init(0), cl::desc("The loop number to map"));
// cl::opt<std::string> munitName("munit", cl::init("na"), cl::desc("the mapping unit name, e.g. : PRE_LN11, INNERMOST_LN11"));
cl::opt<std::string> fName("fn", cl::init("na"), cl::desc("the function name"));
// cl::opt<bool> noName("nn", cl::desc("map all functions and loops"));
// cl::opt<unsigned> initMII("ii", cl::init(0), cl::desc("The starting II for the mapping"));
// cl::opt<unsigned> dimX("dx", cl::init(4), cl::desc("DimX"));
// cl::opt<unsigned> dimY("dy", cl::init(4), cl::desc("DimY"));

cl::opt<unsigned> banks_number ("nobanks", cl::init(2), cl::desc("number of SPM banks"));

cl::opt<unsigned> bank_size ("banksize", cl::init(2048), cl::desc("number of bytes in a bank"));
cl::opt<unsigned> dp_policy ("dppolicy", cl::init(0), cl::desc("data placement policy"));

cl::opt<bool> RemoveAGI(
    "remove-agi", cl::init(false),
    cl::desc("Remove address generation instructions (GEPs and helpers) from DFG"));
cl::opt<std::string> ExternalizeAccess(
    "externalize-access", cl::init("off"),
    cl::desc("Trace-driven access externalization domain: off, agi, or agi_iter"));
cl::opt<unsigned> SIMDPackLanes(
    "simd-pack", cl::init(0),
    cl::desc("Pack complete lane-isomorphic DFG groups after instrumentation"));
cl::opt<std::string> SIMDVectorOps(
    "simd-vector-ops", cl::init(""),
    cl::desc("Comma-separated vector op names supported by the architecture"));
cl::opt<bool> SIMDScalarBroadcast(
    "simd-scalar-broadcast", cl::init(false),
    cl::desc("Allow verified SIMD groups to consume one shared scalar input"));
cl::opt<bool> CanonicalizeLoopBoundaries(
    "canonicalize-loop-boundaries", cl::init(false),
    cl::desc("Replace verified scalar accumulator PHI scaffolding with steady-state recurrence state"));
cl::opt<bool> NativeStatefulMAC(
    "native-stateful-mac", cl::init(false),
    cl::desc("Fuse verified zero-seeded multiply-accumulate recurrences to native MAC"));
cl::opt<bool> TREGResetOnLaunch(
    "treg-reset-on-launch", cl::init(false),
    cl::desc("Target/runtime clears PE-local TREG state before every mapped-loop launch"));
cl::opt<bool> SuppressDFGDot(
    "suppress-dfg-dot", cl::init(false),
    cl::desc("Do not emit DFG DOT files; XML and mem allocation outputs are still emitted"));

STATISTIC(LoopsAnalyzed, "Number of loops analyzed for vectorization");

std::map<std::string, int> sizeArrMap;
std::set<BasicBlock *> LoopBB;
std::map<const BasicBlock *, std::vector<const BasicBlock *>> BBSuccBasicBlocks;
std::map<Loop *, std::vector<BasicBlock *>> loopsExclusieBasicBlockMap;

std::map<std::string, MappingUnit> mappingUnitMap;
std::map<std::string, Loop *> mappingUnit2LoopMap;
std::map<BasicBlock *, std::string> BB2MUnitMap;
std::set<BasicBlock *> nonloopBBs;
LoopTree rootLoop; //current loop is NULL is just a struct capture all the toplevel loops.

std::vector<munitTransition> munitTransitions;
std::vector<munitTransition> munitTransitionsALL;

void traverseDefTree(Instruction *I,
		int depth,
		DFG *currBBDFG, std::map<Instruction *, int> *insMapIn,
		std::map<const BasicBlock *, std::vector<const BasicBlock *>> BBSuccBasicBlocks,
		std::set<BasicBlock *> validBB,
		MemoryDependenceAnalysis *MD)
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

namespace
{

static bool runMorpherDFGGen(Function &F, LoopInfo &LI, ScalarEvolution &SE,
		DependenceInfo &DI, DominatorTree &DT)
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
			ExpandConstantFunnelShifts(F);
			ExpandIntegerMinMaxIntrinsics(F);
			RemoveSelectLeafs(F);
			InsertORtoSingularConditionalBB(F);
			ParseSizeAttr(F, &sizeArrMap);
			ValidateSupportedArrayDimensions(F);

		LLVM_DEBUG(dbgs() << "Processing : " << F.getName() << "\n");

		ScalarEvolution *SEPtr = &SE;
		DependenceInfo *DIPtr = &DI;
		DominatorTree *DTPtr = &DT;
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

		// Loop-peel P/E: pick up the optional remainder marker emitted by
		// `retagRemainderMarkersAsPeel` in the unroll pass. Present only when
		// LLVM's `UnrollLoop` generated a runtime remainder (TC % U != 0).
		// We emit a second DFG XML for it in addition to the kernel.
		std::string peelMunitName = findMappingUnitForOptionalMarker(F, "please_map_me_peel");
		if (!peelMunitName.empty()) {
			errs() << "[dfggen] loop-peel: kernel_munit=" << munitName
				   << " peel_munit=" << peelMunitName << "\n";
		} else {
			LLVM_DEBUG(dbgs() << "[dfggen] loop-peel: no remainder loop\n");
		}

		// Validate loop body for unsupported patterns
		for (BasicBlock *BB : mappingUnitMap[munitName].allBlocks) {
			for (Instruction &I : *BB) {
				if (CallBase *CB = dyn_cast<CallBase>(&I)) {
					Function *calledFn = resolveCalledFunction(CB);
					if (calledFn && !calledFn->isIntrinsic()
					    && calledFn->getName() != "please_map_me"
					    && calledFn->getName() != "please_map_me_peel") {
						errs() << "Warning: function call to '" << calledFn->getName()
							   << "' found in loop body; this may cause issues in DFG generation\n";
					}
				}
			}
		}

		//-----------------------------------
		// Per-phase DFG emission (kernel + optional peel).
		//
		// Phase 2b note: each phase currently runs its own AllocateSPMBanks.
		// Both kernel and peel access the same source-level arrays, so a
		// deterministic allocator that iterates the same array-key set gives
		// the same answer for both phases. However, the inputs to
		// AllocateSPMBanks live in unordered containers keyed on LLVM
		// `Value*`, which differ between the kernel (the unrolled clone)
		// and the peel (a separate clone), and hash-ordering can therefore
		// drift. Phase 2c will lift this to a shared allocation pre-computed
		// across both mapping units; until then peel SPM addresses may not
		// match kernel for the same array. See PLAN.md / REFERENCE.md.
		//-----------------------------------
		auto emitDFGForMappingUnit = [&](const std::string &mname,
		                                  const std::string &kernelNameSuffix) {
			DFGPartPred *LoopDFG = new DFGPartPred(
				F.getName().str() + "_" + mname,
				&loopNames,
				mappingUnitMap[mname].lp);
			if (ExternalizeAccess != "off" && ExternalizeAccess != "agi" &&
				ExternalizeAccess != "agi_iter")
			{
				report_fatal_error(
					"--externalize-access accepts only 'off', 'agi', or 'agi_iter' in the trace-driven pipeline");
			}
			LoopDFG->removeAGI = RemoveAGI || ExternalizeAccess == "agi" ||
				ExternalizeAccess == "agi_iter";
			LoopDFG->iteratorExternalized = ExternalizeAccess == "agi_iter";
			LoopDFG->suppressDOT = SuppressDFGDot;
			LoopDFG->setKernelName(F.getName().str() + kernelNameSuffix);
			LoopDFG->SE = SEPtr;
			LoopDFG->DT = DTPtr;

			LoopDFG->setBBSuccBasicBlocks(BBSuccBasicBlocks);
			LoopDFG->sizeArrMap = sizeArrMap;
			LLVM_DEBUG(dbgs() << "Currently mapping unit : " << mname << "\n");
			assert(!mappingUnitMap[mname].allBlocks.empty());
			LoopDFG->setLoopBB(mappingUnitMap[mname].allBlocks,
					mappingUnitMap[mname].entryBlocks,
					mappingUnitMap[mname].exitBlocks);
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
			LoopDFG->addMemRecDepEdgesNew(DIPtr);
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
				unsigned required_bank_size = bank_size;
				unsigned effective_bank_size = bank_size;
				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][AllocateSPMBanks] begin\n");
				AllocateSPMBanks(
					outVals,
					arrPtrs,
					mem_acceses,
					spm_bank_allocation,
					spm_base_address,
					F,
					required_bank_size,
					effective_bank_size);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][AllocateSPMBanks] end\n\n");
				MEM_SIZE = banks_number * effective_bank_size;

				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][UpdateSPMAllocation] begin\n");
				LoopDFG->UpdateSPMAllocation(spm_base_address,spm_bank_allocation,arrPtrs);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][UpdateSPMAllocation] end\n\n");

				/* UpdateSPMAllocation re-sets GEPbaseAddr (constVal) on all GEP nodes.
				 * For 2-D GEPs whose OLOAD ancestor already carries spm_base+idx*stride,
				 * the ADD must use two register inputs (constValid=false).
				 * Clear GEPbaseAddr for those nodes so the mapper emits constValid=0. */
				LoopDFG->clearGEPbaseAddrForOLOADnodes();
				if (LoopDFG->removeAGI) {
					LoopDFG->removeAGINodes();
					LoopDFG->removeAGI = false;
				}
				LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][InstrumentInOutVars begin]\n");
				LoopDFG->InstrumentInOutVars(F,mem_acceses,OLNodesWithPtrTyUsage,spm_base_address);
				LLVM_DEBUG(dbgs() << "[Skeleton.cpp][InstrumentInOutVars end]\n\n");
				if (CanonicalizeLoopBoundaries && SIMDPackLanes <= 1)
				{
					const int canonicalized =
						LoopDFG->canonicalizeScalarAccumulatorBoundaries(
							NativeStatefulMAC, TREGResetOnLaunch);
					if (canonicalized > 0)
						LoopDFG->classifyParents();
				}
				if (ExternalizeAccess == "agi_iter" && SIMDPackLanes <= 1)
				{
					LoopDFG->externalizeIteratorControl();
					LoopDFG->classifyParents();
				}
				if (SIMDPackLanes <= 1 &&
					LoopDFG->externalizeStationaryWeights() > 0)
					LoopDFG->classifyParents();
				if (SIMDPackLanes > 1)
				{
					std::unordered_set<std::string> vectorOps;
					std::stringstream ops(SIMDVectorOps);
					std::string op;
					while (std::getline(ops, op, ','))
						if (!op.empty()) vectorOps.insert(op);
					if (vectorOps.empty())
						report_fatal_error("--simd-pack requires --simd-vector-ops");
					LoopDFG->packSIMD(SIMDPackLanes, vectorOps, SIMDScalarBroadcast);
					LoopDFG->classifyParents();
				}
			}
			//std::cout << "Code instrumentation done \n";

			LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][PrintOuts] begin\n");
			LoopDFG->PrintOuts();
			LLVM_DEBUG(dbgs() << "\n[Skeleton.cpp][PrintOuts] end\n");
			delete (LoopDFG);
		};

		emitDFGForMappingUnit(munitName, std::string());
		if (!peelMunitName.empty()) {
			emitDFGForMappingUnit(peelMunitName, std::string("_peel"));
		}
		return true;
	  } catch (const DFGGenerationError &e) {
		errs() << "Error: " << e.what() << "\n";
		return false;
	  }
} //END OF runMorpherDFGGen

struct MorpherDFGGenPass : public PassInfoMixin<MorpherDFGGenPass>
{
	PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)
	{
		LoopInfo &LI = AM.getResult<LoopAnalysis>(F);
		ScalarEvolution &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
		DependenceInfo &DI = AM.getResult<DependenceAnalysis>(F);
		DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);

		return runMorpherDFGGen(F, LI, SE, DI, DT)
			? PreservedAnalyses::none()
			: PreservedAnalyses::all();
	}
};

} // namespace

FunctionPassManager createMorpherDFGGenPipeline() {
	FunctionPassManager FPM;
	FPM.addPass(LoopSimplifyPass());
	FPM.addPass(LCSSAPass());
	FPM.addPass(MorpherDFGGenPass());
	return FPM;
}
