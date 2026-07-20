#include "DfgGenInternal.h"

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

void printLoopTree(LoopTree rootLoop, std::map<Loop *, std::string> *loopNames, int tabs);
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

					unsigned effectiveArrayDims = getEffectiveArrayDims(GEP);
					bool isStructFieldAccess = isa<StructType>(GEP->getSourceElementType());
					if (!isStructFieldAccess && effectiveArrayDims > 2)
					{
						std::string displayName = ptrName.empty() ? std::string("<unnamed>") : ptrName;
						DFG_CHECK(false, "UnsupportedArrayDimensions",
							"pointer '" + displayName + "' uses " + std::to_string(effectiveArrayDims) +
							"D indexing in function '" + F.getName().str() +
							"'; Morpher currently supports only up to 2D arrays");
					}
					unsigned pointerDepth = getPointerDepth(GEP->getPointerOperandType());
					if (!isStructFieldAccess && pointerDepth > 2)
					{
						std::string displayName = ptrName.empty() ? std::string("<unnamed>") : ptrName;
						DFG_CHECK(false, "UnsupportedArrayDimensions",
							"pointer '" + displayName + "' has pointer depth " + std::to_string(pointerDepth) +
							" in function '" + F.getName().str() +
							"'; Morpher currently supports only up to 2D arrays");
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
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	auto traceEndFn = F.getParent()->getOrInsertFunction(
			"loopTraceClose",
			FunctionType::getVoidTy(Ctx));

	auto loopInvFn = F.getParent()->getOrInsertFunction(
			"loopInvoke",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	auto loopInsUpdateFn = F.getParent()->getOrInsertFunction(
			"loopInsUpdate",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			Type::getInt32Ty(Ctx));

	auto loopBBInsUpdateFn = F.getParent()->getOrInsertFunction(
			"loopBBInsUpdate",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			Type::getInt32Ty(Ctx));

	auto loopInsClearFn = F.getParent()->getOrInsertFunction(
			"loopInsClear",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	auto loopBBInsClearFn = F.getParent()->getOrInsertFunction(
			"loopBBInsClear",
			FunctionType::getVoidTy(Ctx));

	auto loopInvokeEndFn = F.getParent()->getOrInsertFunction(
			"loopInvokeEnd",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	auto reportExecInsCountFn = F.getParent()->getOrInsertFunction(
			"reportExecInsCount",
			FunctionType::getVoidTy(Ctx),
			Type::getInt32Ty(Ctx));

	auto updateLoopPreHeaderFn = F.getParent()->getOrInsertFunction(
			"updateLoopPreHeader",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	auto loopBBMappingUnitUpdate = F.getParent()->getOrInsertFunction(
			"loopBBMappingUnitUpdate",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	auto recordUncondMunitTransition = F.getParent()->getOrInsertFunction(
			"recordUncondMunitTransition",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	auto recordCondMunitTransition = F.getParent()->getOrInsertFunction(
			"recordCondMunitTransition",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx),
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
			int instructionCountBB = BB->size();
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

		int instructionCountBB = lpPreHeaderBB->size();
		Value *instructionCountBBVal = ConstantInt::get(Type::getInt32Ty(Ctx), instructionCountBB);
		Value *BBName = builder.CreateGlobalStringPtr(lpPreHeaderBB->getName());

		builder.SetInsertPoint(lpPreHeaderBB, --lpPreHeaderBB->end());
		builder.CreateCall(loopBBInsUpdateFn, {loopName, BBName, instructionCountBBVal});
	}

	//find return instruction
	for (BasicBlock &BB : F)
	{
		Value *insCountBBVal = ConstantInt::get(Type::getInt32Ty(Ctx), BB.size());
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
			if (CallBase *CB = dyn_cast<CallBase>(&I))
			{
				Function *calledFn = resolveCalledFunction(CB);
				if (calledFn && calledFn->getName() == "please_map_me")
				{
					LLVM_DEBUG(dbgs()  << "token found in BB = " << BB.getName() << "\n");
					MUBB = &BB;
					checker_ins = cast<Instruction>(CB);
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

// Optional variant: returns the mapping unit name owning a call to `markerName`
// (e.g. "please_map_me_peel"), or empty string if no such call exists. Used by
// the loop-peel P/E path -- the peel marker is only present when the LLVM
// unroll pass detected a non-divisible trip count and converted the remainder
// loop's marker (see retagRemainderMarkersAsPeel in morpherLoopUnrollPass.cpp).
// Erases the marker call on success so it does not contaminate later codegen.
std::string findMappingUnitForOptionalMarker(Function &F, const std::string &markerName)
{
	BasicBlock *MUBB = nullptr;
	Instruction *checker_ins = nullptr;
	for (auto &BB : F)
	{
		for (auto &I : BB)
		{
			if (CallBase *CB = dyn_cast<CallBase>(&I))
			{
				Function *calledFn = resolveCalledFunction(CB);
				if (calledFn && calledFn->getName() == markerName)
				{
					MUBB = &BB;
					checker_ins = cast<Instruction>(CB);
				}
			}
		}
	}
	if (!MUBB || !checker_ins)
		return std::string();
	checker_ins->eraseFromParent();
	for (auto it = mappingUnitMap.begin(); it != mappingUnitMap.end(); it++)
	{
		if (it->second.allBlocks.count(MUBB) > 0)
			return it->first;
	}
	return std::string();
}
