#include "DfgCoreInternal.h"

int DFG::handlePHINodes(std::set<BasicBlock *> LoopBB)
{
	LLVM_DEBUG(dbgs() << "handlePHINodes started!\n");
	dfgNode *node;
	std::map<dfgNode *, std::vector<const BasicBlock *>> processedPhiNodes;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		for (int j = 0; j < node->PHIchildren.size(); ++j)
		{
			assert(node->PHIchildren[j] != NULL);
			LLVM_DEBUG(node->PHIchildren[j]->dump());
			assert(findNode(node->PHIchildren[j]) != NULL);

			LLVM_DEBUG(node->PHIchildren[j]->dump());

			if (PHINode *phiIns = dyn_cast<PHINode>(node->PHIchildren[j]))
			{
				for (int k = 0; k < phiIns->getNumIncomingValues(); ++k)
				{
					BasicBlock *bb = phiIns->getIncomingBlock(k);
					if (bb != node->BB)
					{
						continue;
					}
					LLVM_DEBUG(dbgs() << "handlePHINodes adding nodes...\n");
					BasicBlock::iterator instIter = --bb->end();
					Instruction *brIns = &*instIter;
					assert(brIns->getOpcode() == Instruction::Br);

					dfgNode *brNode = findNode(brIns);
					findNode(node->PHIchildren[j])->addCMergeParent(node, brNode);
					processedPhiNodes[findNode(node->PHIchildren[j])].push_back(node->BB);
				}
			}
			else
			{
				assert(0);
			}

			//			node->addPHIChildNode(findNode(node->PHIchildren[j]));
			//			findNode(node->PHIchildren[j])->addPHIAncestorNode(node);
		}
	}

	LLVM_DEBUG(dbgs() << "second loop\n");

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getNode() == NULL)
			continue;
		if (PHINode *phiIns = dyn_cast<PHINode>(node->getNode()))
		{
			LLVM_DEBUG(phiIns->dump());
			LLVM_DEBUG(dbgs() << "incomingblocks:" << phiIns->getNumIncomingValues() << "\n");

			for (int k = 0; k < phiIns->getNumIncomingValues(); ++k)
			{
				BasicBlock *bb = phiIns->getIncomingBlock(k);

				if (processedPhiNodes.find(node) != processedPhiNodes.end())
				{
					if (std::find(processedPhiNodes[node].begin(),
							processedPhiNodes[node].end(),
							bb) != processedPhiNodes[node].end())
					{
						continue;
					}
				}

				LLVM_DEBUG(dbgs() << "handlePHINodes adding nodes in second loop...\n");

				BasicBlock::iterator instIter = --bb->end();
				Instruction *brIns = &*instIter;
				LLVM_DEBUG(brIns->dump());
				//				assert(LoopBB.find(brIns->getParent())!=LoopBB.end());
				assert(brIns->getOpcode() == Instruction::Br);
				dfgNode *brNode = findNode(brIns);
				//				assert(brNode!=NULL);

				if (ConstantInt *CI = dyn_cast<ConstantInt>(phiIns->getIncomingValueForBlock(bb)))
				{
					assert(CI->getBitWidth() <= 32);

					if (LoopBB.find(brIns->getParent()) == LoopBB.end())
					{
						node->addCMergeParent(brIns, CI->getSExtValue());
					}
					else
					{
						assert(brNode != NULL);
						node->addCMergeParent(brNode, NULL, CI->getSExtValue());
					}

				} //TODO : DAC18
				else if (ConstantFP *CF = dyn_cast<ConstantFP>(phiIns->getIncomingValueForBlock(bb)))
				{
					//					assert(CI->getBitWidth() <= 32);

					if (LoopBB.find(brIns->getParent()) == LoopBB.end())
					{
						node->addCMergeParent(brIns, (int)(CF->getValueAPF().convertToFloat()));
					}
					else
					{
						assert(brNode != NULL);
						node->addCMergeParent(brNode, NULL, (int)(CF->getValueAPF().convertToFloat()));
					}

				} //TODO : DAC18
				else if (UndefValue *UND = dyn_cast<UndefValue>(phiIns->getIncomingValueForBlock(bb)))
				{

					if (LoopBB.find(brIns->getParent()) == LoopBB.end())
					{
						node->addCMergeParent(brIns, 0);
					}
					else
					{
						assert(brNode != NULL);
						node->addCMergeParent(brNode, NULL, 0);
					}
				}
				else if (Instruction *phiData = dyn_cast<Instruction>(phiIns->getIncomingValueForBlock(bb)))
				{
					if (LoopBB.find(phiData->getParent()) == LoopBB.end())
					{
						//						node->addLoadParent(phiData);
						//						assert(OutLoopNodeMap[phiData]!=NULL);
						//						dfgNode* phiDataNode=OutLoopNodeMap[phiData];
						//						node->addCMergeParent(brNode,phiDataNode);

						if (LoopBB.find(brIns->getParent()) == LoopBB.end())
						{
							node->addCMergeParent(brIns, phiData);
						}
						else
						{
							node->addCMergeParent(brNode, phiData);
						}
					}
					else
					{
						assert(findNode(phiData) != NULL);
						//						assert(brNode!=NULL);
						dfgNode *phiDataNode = findNode(phiData);
						//						node->addCMergeParent(brNode,phiDataNode);

						if (LoopBB.find(brIns->getParent()) == LoopBB.end())
						{
							node->addCMergeParent(brIns, phiDataNode);
						}
						else
						{
							node->addCMergeParent(brNode, phiDataNode);
						}
					}
				}
				else if (sizeArrMap.find(phiIns->getIncomingValueForBlock(bb)->getName().str()) != sizeArrMap.end())
				{

					std::string ptrName = phiIns->getIncomingValueForBlock(bb)->getName().str();

					if (LoopBB.find(brIns->getParent()) == LoopBB.end())
					{
						node->addCMergeParent(brIns, allocatedArraysMap[ptrName]);
					}
					else
					{
						assert(brNode != NULL);
						node->addCMergeParent(brNode, NULL, allocatedArraysMap[ptrName]);
					}
				}
				else
				{
					for (std::pair<std::string, int> pair : sizeArrMap)
					{
						LLVM_DEBUG(dbgs() << pair.first << ":" << pair.second << "\n");
					}
					LLVM_DEBUG(dbgs() << "sizeARRMap size =" << sizeArrMap.size() << "\n");
					LLVM_DEBUG(phiIns->getIncomingValueForBlock(bb)->dump());
					assert(0);
				}
			}
		}
	}

	bool loopStartFound = false;
	for (std::pair<BasicBlock *, BasicBlock *> pair : this->loopentryBB)
	{
		BasicBlock *bb = pair.first;
		BasicBlock *entrybb = pair.second;
		BasicBlock::iterator instIter = --bb->end();
		Instruction *brIns = &*instIter;
		assert(brIns->getOpcode() == Instruction::Br);

		if (LoopStartMap.find(brIns) != LoopStartMap.end())
		{
			continue;
		}

		dfgNode *tempLoopStartNode;
		tempLoopStartNode = new dfgNode(this);
		if (LoopStartMap.empty())
		{
			tempLoopStartNode->setIdx(getNodesPtr()->size());
			getNodesPtr()->push_back(tempLoopStartNode);
		}
		else
		{
			tempLoopStartNode->setIdx(10000);
		}
		tempLoopStartNode->setNameType("LOOPSTART");
		LLVM_DEBUG(dbgs() << "Adding loopstart.\n");
		LLVM_DEBUG(brIns->dump());
		tempLoopStartNode->BB = entrybb;
		LoopStartMap[brIns] = tempLoopStartNode;

		std::vector<dfgNode *> leafNodes;
		for (int i = 0; i < NodeList.size(); ++i)
		{
			node = NodeList[i];
			if (node->getNameType().compare("LOOPSTART") == 0)
			{
				continue;
			}
			if (node->getNameType().compare("OutLoopLOAD") == 0)
			{
				continue;
			}
			if (node->getNameType().compare("MOVC") == 0)
			{
				continue;
			}
			if (node->BB != entrybb)
			{
				continue;
			}
			if (node->getAncestors().empty() && node->getPHIancestors().empty())
			{
				leafNodes.push_back(node);
			}
		}

		for (dfgNode *leafNode : leafNodes)
		{
			tempLoopStartNode->addChildNode(leafNode);
			leafNode->addAncestorNode(tempLoopStartNode);
		}
	}

	LLVM_DEBUG(dbgs() << "handlePHINodes DONE!\n");
	return 0;
}

TreePath DFG::createTreePathPHIDest(dfgNode *node, CGRANode *dest, CGRANode *phiChildDest)
{
	TreePath tp;
	dfgNode *child;
	CGRANode *NodeExt;
	CGRANode *cnode;
	int MII = currCGRA->getMII();

	assert(!node->getPHIchildren().empty());

	if (dest->getPEType() == MEM)
	{
		NodeExt = currCGRA->getCGRANode((dest->getT() + 2) % MII, dest->getY(), dest->getX());
		tp.sourceSCpathTdimLengths[NodeExt] = 2;
	}
	else
	{
		NodeExt = currCGRA->getCGRANode((dest->getT() + 1) % MII, dest->getY(), dest->getX());
		tp.sourceSCpathTdimLengths[NodeExt] = 1;
	}
	tp.sources.push_back(NodeExt);
	tp.sourcePorts[NodeExt] = TILE;
	tp.sourcePaths[NodeExt] = (std::make_pair(node, node));
	tp.sourceSCpathLengths[NodeExt] = 0;

	assert(node != NULL);

	tp.dest = phiChildDest;
	bool foundParentTreeBasedRoutingLocs = false;

	return tp;
}

void DFG::addPHIChildEdges()
{
	dfgNode *node;
	dfgNode *phiChildNode;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (!node->PHIchildren.empty())
		{
			for (int j = 0; j < node->PHIchildren.size(); ++j)
			{
				phiChildNode = findNode(node->PHIchildren[j]);

				Edge temp;
				temp.setID(getEdges().size());

				std::ostringstream ss;
				ss << std::dec << node->getIdx() << "_to_" << phiChildNode->getIdx();
				temp.setName(ss.str());
				temp.setType(EDGE_TYPE_PHI);
				temp.setSrc(node);
				temp.setDest(phiChildNode);

				InsertEdge(temp);
			}
		}
	}
}

void DFG::addPHIParents()
{
	dfgNode *node;
	dfgNode *phiChild;

	std::vector<dfgNode *> ancestors;
	std::vector<dfgNode *>::iterator searchParent;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		for (int j = 0; j < node->getPHIchildren().size(); ++j)
		{
			phiChild = node->getPHIchildren()[j];
			ancestors = phiChild->getAncestors();
			LLVM_DEBUG(dbgs() << "\n addPHIParents : child : " << phiChild->getIdx() << "\n");
			LLVM_DEBUG(dbgs() << "addPHIParents : anc : " << node->getIdx() << "\n");
			LLVM_DEBUG(dbgs() << "ancestors.size= : " << ancestors.size() << "\n");
			searchParent = std::find(ancestors.begin(), ancestors.end(), node);
			if (searchParent != ancestors.end())
				LLVM_DEBUG(dbgs() << "searchParent : " << (*searchParent)->getIdx() << "\n");
			if (searchParent == ancestors.end())
			{
				LLVM_DEBUG(dbgs() << "ifaddPHIParents : child : " << phiChild->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "ifaddPHIParents : anc : " << node->getIdx() << "\n");
				phiChild->addAncestorNode(node, EDGE_TYPE_PHI);
				node->addChildNode(phiChild, EDGE_TYPE_PHI);
			}
		}
	}
}

dfgNode *DFG::findNodeMappedLoc(CGRANode *cnode)
{
	dfgNode *node;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getMappedLoc() == cnode)
		{
			return node;
		}
	}
	return NULL;
}

void DFG::partitionFuncDFG(DFG *funcDFG, std::vector<DFG *> dfgVectorPtr)
{
}

void DFG::GEPInvestigate(Function &F, Loop *L, std::map<std::string, int> *sizeArrMap)
{

	LLVMContext &Ctx = F.getContext();
	std::set<Value *> handledGEPs;

	//LoopPreHeader Printf marker insertion
	BasicBlock *loopPH = L->getLoopPreheader();
	BasicBlock::iterator instIterPH = loopPH->begin();
	Instruction *loopPHStartIns = &*instIterPH;

	IRBuilder<> builder(loopPHStartIns);
	builder.SetInsertPoint(loopPH, loopPH->getFirstInsertionPt());

	Value *printfSTARTstr = builder.CreateGlobalStringPtr((*this->loopNamesPtr)[L] + ":------------LOOP START--------------\n");
	Value *printfENDstr = builder.CreateGlobalStringPtr((*this->loopNamesPtr)[L] + ":------------LOOP END----------------\n");
	assert(this->loopNamesPtr->find(L) != this->loopNamesPtr->end());
	Value *loopName = builder.CreateGlobalStringPtr((*this->loopNamesPtr)[L]);

	//	Constant* printfMARKER = F.getParent()->getOrInsertFunction(
	//					  "printf",
	//					  FunctionType::getVoidTy(Ctx),
	//					  Type::getInt8PtrTy(Ctx),
	//					  NULL);

	auto loopStartFn = F.getParent()->getOrInsertFunction(
			"loopStart",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	builder.CreateCall(loopStartFn, {loopName});

	//the entry location for instrumentation code
	BasicBlock *loopHeader = L->getHeader();
	BasicBlock::iterator instIter = loopHeader->begin();
	Instruction *loopStartIns = &*instIter;

	SmallVector<BasicBlock *, 8> loopExitBlocksSV;
	L->getExitBlocks(loopExitBlocksSV);

	std::set<BasicBlock *> s;
	for (unsigned i = 0; i < loopExitBlocksSV.size(); ++i)
		s.insert(loopExitBlocksSV[i]);
	std::vector<BasicBlock *> loopExitBlocks;
	loopExitBlocks.assign(s.begin(), s.end());

	std::vector<Instruction *> loopExitInsVec;
	assert(loopExitBlocks.size() != 0);
	for (int i = 0; i < loopExitBlocks.size(); ++i)
	{
		BasicBlock *LoopExitBB = loopExitBlocks[i];
		//		BasicBlock::iterator instIter = --LoopExitBB->end();
		BasicBlock::iterator instIter = LoopExitBB->begin();
		LLVM_DEBUG(dbgs() << "LoopExit Blocks : \n");
		LLVM_DEBUG(LoopExitBB->dump());

		if (std::find(loopExitInsVec.begin(), loopExitInsVec.end(), &*instIter) == loopExitInsVec.end())
		{
			loopExitInsVec.push_back(&*instIter);
		}
	}

	//	IRBuilder<> builder(loopStartIns);
	//	builder.SetInsertPoint(loopStartIns);
	builder.SetInsertPoint(loopHeader, loopHeader->getFirstInsertionPt());
	auto clearPrintedArrs = F.getParent()->getOrInsertFunction(
			"clearPrintedArrs", FunctionType::getVoidTy(Ctx));

	//	for (int i = 0; i < loopExitInsVec.size(); ++i) {
	//		builder.SetInsertPoint(loopExitInsVec[i]);
	//		builder.CreateCall(clearPrintedArrs);
	//	}

	for (int i = 0; i < loopExitBlocks.size(); ++i)
	{
		builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
		builder.CreateCall(clearPrintedArrs);
	}

	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		if (node->getNameType().compare("OutLoopLOAD") == 0)
		{
			if (!node->isTransferedByHost())
			{
				continue;
			}
			LLVM_DEBUG(dbgs() << "OutLoopLoad found!!\n");
			Value *printfstr = builder.CreateGlobalStringPtr("OutLoopLoadNode:%d,val=%d,addr=%d\n");
			assert(OutLoopNodeMapReverse[node] != NULL);
			LLVM_DEBUG(OutLoopNodeMapReverse[node]->dump());
			Value *loadVal = OutLoopNodeMapReverse[node];
			Value *nodeIdx = ConstantInt::get(Type::getInt32Ty(Ctx), node->getIdx());
			Value *addrVal = ConstantInt::get(Type::getInt32Ty(Ctx), node->getoutloopAddr());
			//			Value* args[] = {printfstr,nodeIdx,loadVal,addrVal};

			//			Constant* printf = F.getParent()->getOrInsertFunction(
			//							  "printf",
			//							  FunctionType::getVoidTy(Ctx),
			//							  Type::getInt8PtrTy(Ctx),
			//							  Type::getInt32Ty(Ctx),
			//							  loadVal->getType(),
			//							  Type::getInt32Ty(Ctx),
			//							  NULL);

			auto outloopReportFn = F.getParent()->getOrInsertFunction(
					"outloopValueReport",
					FunctionType::getVoidTy(Ctx),
					Type::getInt32Ty(Ctx), //nodeIdx
					loadVal->getType(),	//value
					Type::getInt32Ty(Ctx), //addr
					Type::getInt8Ty(Ctx),  //isLoad
					Type::getInt8Ty(Ctx),  //isHostTrans
					Type::getInt8Ty(Ctx)   //size
			);

			Value *isLoad = ConstantInt::get(Type::getInt8Ty(Ctx), 1);
			Value *isHostTrans;
			Value *size = ConstantInt::get(Type::getInt8Ty(Ctx), node->getTypeSizeBytes());
			if (node->isTransferedByHost())
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 1);
			}
			else
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 0);
			}
			Value *args[] = {nodeIdx, loadVal, addrVal, isLoad, isHostTrans, size};
			for (int i = 0; i < loopExitBlocks.size(); ++i)
			{
				builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
				//				builder.CreateCall(printf,args);
				builder.CreateCall(outloopReportFn, args);
				//				LLVM_DEBUG(dbgs() << "its a load\n";
				//				loopExitBlocks[i]->dump();
			}
		}

		if (node->getNameType().compare("OutLoopSTORE") == 0)
		{
			if (!node->isTransferedByHost())
			{
				continue;
			}
			Value *printfstr = builder.CreateGlobalStringPtr("OutLoopStoreNode:%d,val=%d,addr=%d\n");
			assert(node->getAncestors().size() == 1);
			Value *StoreVal = (node->getAncestors())[0]->getNode();
			Value *nodeIdx = ConstantInt::get(Type::getInt32Ty(Ctx), node->getIdx());
			Value *addrVal = ConstantInt::get(Type::getInt32Ty(Ctx), node->getoutloopAddr());
			//			Value* args[] = {printfstr,nodeIdx,StoreVal,addrVal};

			//			Constant* printf = F.getParent()->getOrInsertFunction(
			//							  "printf",
			//							  FunctionType::getVoidTy(Ctx),
			//							  Type::getInt8PtrTy(Ctx),
			//							  Type::getInt32Ty(Ctx),
			//							  StoreVal->getType(),
			//							  Type::getInt32Ty(Ctx),
			//							  NULL);

			auto outloopReportFn = F.getParent()->getOrInsertFunction(
					"outloopValueReport",
					FunctionType::getVoidTy(Ctx),
					Type::getInt32Ty(Ctx), //nodeIdx
					StoreVal->getType(),   //value
					Type::getInt32Ty(Ctx), //addr
					Type::getInt8Ty(Ctx),  //isLoad
					Type::getInt8Ty(Ctx),  //isHostTrans
					Type::getInt8Ty(Ctx)   //size
			);

			Value *isLoad = ConstantInt::get(Type::getInt8Ty(Ctx), 0);
			Value *isHostTrans;
			Value *size = ConstantInt::get(Type::getInt8Ty(Ctx), node->getTypeSizeBytes());
			if (node->isTransferedByHost())
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 1);
			}
			else
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 0);
			}
			Value *args[] = {nodeIdx, StoreVal, addrVal, isLoad, isHostTrans, size};
			for (int i = 0; i < loopExitBlocks.size(); ++i)
			{
				builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
				//				builder.CreateCall(printf,args);
				builder.CreateCall(outloopReportFn, args);
				//				LLVM_DEBUG(dbgs() << "its a store\n";
				//				OutLoopNodeMapReverse[node]->dump();
				//				loopExitBlocks[i]->dump();
			}
		}

		if (node->getNode() == NULL)
		{
			continue;
		}
		Instruction *ins = node->getNode();

		//		for (auto &B : F){
		//			BasicBlock* BB = dyn_cast<BasicBlock>(&B);
		//			for(auto &I : *BB){
		//
		//			Instruction* ins = &I;

		//			LLVM_DEBUG(dbgs() << "GEPInvestigate node found\n";

		if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(ins))
		{
			const DataLayout DL = ins->getParent()->getParent()->getParent()->getDataLayout();

			//number of elements
			LLVM_DEBUG(dbgs() << "Pointer operand = " << GEP->getPointerOperand()->getName() << "\n");
			LLVM_DEBUG(GEP->dump());
			Type *T = GEP->getSourceElementType();

			LLVM_DEBUG(dbgs() << "S/A/I/P=" << T->isStructTy() << "/");
			LLVM_DEBUG(dbgs() << T->isArrayTy() << "/");
			LLVM_DEBUG(dbgs() << T->isIntegerTy() << "/");
			LLVM_DEBUG(dbgs() << T->isPointerTy() << "\n");

			if (dyn_cast<StructType>(T))
			{

				if (handledGEPs.find(GEP->getPointerOperand()) != handledGEPs.end())
				{
					continue;
				}

				StructType *ST = dyn_cast<StructType>(T);
				LLVM_DEBUG(dbgs() << "StructType=" << ST->getName() << "\n");

				//				//Insert a call to our function
				//				builder(loopStartIns);
				//				builder.SetInsertPoint(loopStartIns);
				builder.SetInsertPoint(loopHeader, loopHeader->getFirstInsertionPt());
				//				builder.SetInsertPoint(loopHeader,++builder.GetInsertPoint());

				Value *old = GEP->getPointerOperand();
				Value *bitcastedPtr = builder.CreateBitCast(old, morpher::llvm_compat::getInt8PtrTy(Ctx));
				int size = DL.getTypeAllocSize(ST);

				(*sizeArrMap)[GEP->getPointerOperand()->getName().str()] = size;

				if (allocatedArraysMap.find(GEP->getPointerOperand()->getName().str()) == allocatedArraysMap.end())
				{
					dfgNode *memopChild = node->getChildren()[0];
					assert(memopChild->getLeftAlignedMemOp() != 0);
					if (memopChild->getLeftAlignedMemOp() == 1)
					{
						node->setGEPbaseAddr(arrayAddrPtrLeft);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrLeft << ",size=" << size << "\n");
						allocatedArraysMap[GEP->getPointerOperand()->getName().str()] = arrayAddrPtrLeft;
						arrayAddrPtrLeft += size;
					}
					else
					{
						node->setGEPbaseAddr(arrayAddrPtrRight);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrRight << ",size=" << size << "\n");
						allocatedArraysMap[GEP->getPointerOperand()->getName().str()] = arrayAddrPtrRight;
						arrayAddrPtrRight += size;
					}
				}
				else
				{
					node->setGEPbaseAddr(allocatedArraysMap[GEP->getPointerOperand()->getName().str()]);
					LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << allocatedArraysMap[GEP->getPointerOperand()->getName().str()] << ",size=" << size << "\n");
				}

				auto printArrFunc = F.getParent()->getOrInsertFunction(
						"printArr",
						FunctionType::getVoidTy(Ctx),
						morpher::llvm_compat::getInt8PtrTy(Ctx),
						morpher::llvm_compat::getInt8PtrTy(Ctx),
						Type::getInt32Ty(Ctx),
						Type::getInt8Ty(Ctx),
						Type::getInt32Ty(Ctx));

				Value *st_name = builder.CreateGlobalStringPtr(ST->getName());
				Value *argsi[] = {st_name,
						bitcastedPtr,
						ConstantInt::get(Type::getInt32Ty(Ctx), size),
						ConstantInt::get(Type::getInt8Ty(Ctx), 1),
						ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
				Value *argso[] = {st_name,
						bitcastedPtr,
						ConstantInt::get(Type::getInt32Ty(Ctx), size),
						ConstantInt::get(Type::getInt8Ty(Ctx), 0),
						ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
				//				builder.CreateCall(printArrFunc,args);

				builder.CreateCall(printArrFunc, argsi);

				//				for (int i = 0; i < loopExitInsVec.size(); ++i) {
				//					builder.SetInsertPoint(loopExitInsVec[i]);
				//					builder.CreateCall(printArrFunc,argso);
				//				}

				for (int i = 0; i < loopExitBlocks.size(); ++i)
				{
					builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
					builder.CreateCall(printArrFunc, argso);
				}

				//experiment
				//				LLVM_DEBUG(dbgs() << "Experiment, arraytype size = " << AT->getArrayNumElements() << "\n";
			}
			else
			{
				if (dyn_cast<ArrayType>(T))
				{

					if (handledGEPs.find(GEP->getPointerOperand()) != handledGEPs.end())
					{
						continue;
					}

					ArrayType *AT = dyn_cast<ArrayType>(T);
					LLVM_DEBUG(dbgs() << "ArrayType=" << AT->getArrayNumElements() << "\n");
					LLVM_DEBUG(dbgs() << "Size = " << DL.getTypeAllocSize(AT) << "\n");

					//					//Insert a call to our function
					//					IRBuilder<> builder(loopStartIns);
					//					builder.SetInsertPoint(loopStartIns);
					builder.SetInsertPoint(loopHeader, loopHeader->getFirstInsertionPt());

					Value *old = GEP->getPointerOperand();
					Value *bitcastedPtr = builder.CreateBitCast(old, morpher::llvm_compat::getInt8PtrTy(Ctx));
					int size = DL.getTypeAllocSize(AT);

					(*sizeArrMap)[GEP->getPointerOperand()->getName().str()] = size;
					dfgNode *memopChild = node->getChildren()[0];
					assert(memopChild->getLeftAlignedMemOp() != 0);
					if (memopChild->getLeftAlignedMemOp() == 1)
					{
						node->setGEPbaseAddr(arrayAddrPtrLeft);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrLeft << ",size=" << size << "\n");
						arrayAddrPtrLeft += size;
					}
					else
					{
						node->setGEPbaseAddr(arrayAddrPtrRight);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrRight << ",size=" << size << "\n");
						arrayAddrPtrRight += size;
					}

					auto printArrFunc = F.getParent()->getOrInsertFunction(
							"printArr",
							FunctionType::getVoidTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							Type::getInt32Ty(Ctx),
							Type::getInt8Ty(Ctx),
							Type::getInt32Ty(Ctx));

					Value *st_name = builder.CreateGlobalStringPtr(GEP->getPointerOperand()->getName());
					Value *argsi[] = {st_name,
							bitcastedPtr,
							ConstantInt::get(Type::getInt32Ty(Ctx), size),
							ConstantInt::get(Type::getInt8Ty(Ctx), 1),
							ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
					Value *argso[] = {st_name,
							bitcastedPtr,
							ConstantInt::get(Type::getInt32Ty(Ctx), size),
							ConstantInt::get(Type::getInt8Ty(Ctx), 0),
							ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};

					//Add a call in the begginning of the loop
					builder.CreateCall(printArrFunc, argsi);

					//					LLVM_DEBUG(dbgs() << "loopExitInsVec.size()=" << loopExitInsVec.size() << "\n";
					//					for (int i = 0; i < loopExitInsVec.size(); ++i) {
					//						loopExitInsVec[i]->dump();
					//						builder.SetInsertPoint(loopExitInsVec[i]);
					//						builder.CreateCall(printArrFunc,argso);
					//					}

					for (int i = 0; i < loopExitBlocks.size(); ++i)
					{
						builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
						builder.CreateCall(printArrFunc, argso);
					}
				}
				else
				{
					assert(dyn_cast<IntegerType>(T));
					IntegerType *IT = dyn_cast<IntegerType>(T);

					//					IRBuilder<> builder(GEP);
					builder.SetInsertPoint(GEP);
					//					builder.SetInsertPoint(loopHeader,++builder.GetInsertPoint());

					Value *old = GEP->getPointerOperand();
					Value *addr = GEP;
					Value *bitcastedPtr = builder.CreateBitCast(old, morpher::llvm_compat::getInt8PtrTy(Ctx));
					int size = DL.getTypeAllocSize(IT);

					auto reportDynArrSize = F.getParent()->getOrInsertFunction(
							"reportDynArrSize", FunctionType::getVoidTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							Type::getInt32Ty(Ctx),
							Type::getInt32Ty(Ctx));

					Value *st_name = builder.CreateGlobalStringPtr(GEP->getPointerOperand()->getName());
					Value *args[] = {st_name,
							bitcastedPtr,
							GEP->getOperand(1),
							ConstantInt::get(Type::getInt32Ty(Ctx),
									size)};
					builder.CreateCall(reportDynArrSize, args);

					std::string ptrName = GEP->getPointerOperand()->getName().str();

					//checking for user defined input sizes
					if (sizeArrMap->find(ptrName) != sizeArrMap->end())
					{
						//						builder.SetInsertPoint(loopStartIns);
						//						builder.SetInsertPoint(loopHeader,++builder.GetInsertPoint());
						int size = (*sizeArrMap)[ptrName];

						if (allocatedArraysMap.find(ptrName) == allocatedArraysMap.end())
						{
							dfgNode *memopChild = node->getChildren()[0];
							assert(memopChild->getLeftAlignedMemOp() != 0);
							if (memopChild->getLeftAlignedMemOp() == 1)
							{
								node->setGEPbaseAddr(arrayAddrPtrLeft);
								LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrLeft << ",size=" << size << "\n");
								allocatedArraysMap[ptrName] = arrayAddrPtrLeft;
								arrayAddrPtrLeft += size;
							}
							else
							{
								node->setGEPbaseAddr(arrayAddrPtrRight);
								LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrRight << ",size=" << size << "\n");
								allocatedArraysMap[ptrName] = arrayAddrPtrRight;
								arrayAddrPtrRight += size;
							}
						}
						else
						{
							node->setGEPbaseAddr(allocatedArraysMap[ptrName]);
							LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << allocatedArraysMap[ptrName] << ",size=" << size << "\n");
						}

						auto printArrFunc = F.getParent()->getOrInsertFunction(
								"printArr",
								FunctionType::getVoidTy(Ctx),
								morpher::llvm_compat::getInt8PtrTy(Ctx),
								morpher::llvm_compat::getInt8PtrTy(Ctx),
								Type::getInt32Ty(Ctx),
								Type::getInt8Ty(Ctx),
								Type::getInt32Ty(Ctx));

						Value *argsi[] = {st_name,
								bitcastedPtr,
								ConstantInt::get(Type::getInt32Ty(Ctx), size),
								ConstantInt::get(Type::getInt8Ty(Ctx), 1),
								ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
						Value *argso[] = {st_name,
								bitcastedPtr,
								ConstantInt::get(Type::getInt32Ty(Ctx), size),
								ConstantInt::get(Type::getInt8Ty(Ctx), 0),
								ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
						//Add a call in the begginning of the loop
						builder.CreateCall(printArrFunc, argsi);
						//
						//						LLVM_DEBUG(dbgs() << "loopExitInsVec.size()=" << loopExitInsVec.size() << "\n";
						//						for (int i = 0; i < loopExitInsVec.size(); ++i) {
						//							loopExitInsVec[i]->dump();
						//							builder.SetInsertPoint(loopExitInsVec[i]);
						//							builder.CreateCall(printArrFunc,argso);
						//						}

						for (int i = 0; i < loopExitBlocks.size(); ++i)
						{
							builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
							builder.CreateCall(printArrFunc, argso);
						}
					}
					else
					{
						LLVM_DEBUG(dbgs() << "Please provide sizes for the arrayptr : " << ptrName << "\n");
						assert(0);
					}
				}
			}

			LLVM_DEBUG(dbgs() << "GEPInvestigate : instrument code added!\n");
			handledGEPs.insert(GEP->getPointerOperand());
		}

		//		} // for(auto &I : *BB){
		//	} //for (auto &B : F){

	} //Nodelist

	auto printDynArrSize = F.getParent()->getOrInsertFunction(
			"printDynArrSize", FunctionType::getVoidTy(Ctx));

	auto loopEndFn = F.getParent()->getOrInsertFunction(
			"loopEnd", FunctionType::getVoidTy(Ctx), morpher::llvm_compat::getInt8PtrTy(Ctx));

	//	for (int i = 0; i < loopExitInsVec.size(); ++i) {
	//		builder.SetInsertPoint(loopExitInsVec[i]);
	//		builder.CreateCall(clearPrintedArrs);
	//		builder.CreateCall(loopEndFn,{loopName});
	//	}

	for (int i = 0; i < loopExitBlocks.size(); ++i)
	{
		builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
		builder.CreateCall(clearPrintedArrs);
		builder.CreateCall(loopEndFn, {loopName});
	}

} //End of GEPInvestigate Function

//2017 New version for mapping unit

void DFG::GEPInvestigate(Function &F, std::map<std::string, int> *sizeArrMap)
{

	LLVMContext &Ctx = F.getContext();
	std::set<Value *> handledGEPs;

	//LoopPreHeader Printf marker insertion
	BasicBlock *loopPH = L->getLoopPreheader();
	BasicBlock::iterator instIterPH = loopPH->begin();
	Instruction *loopPHStartIns = &*instIterPH;

	IRBuilder<> builder(loopPHStartIns);
	builder.SetInsertPoint(loopPH, loopPH->getFirstInsertionPt());

	Value *printfSTARTstr = builder.CreateGlobalStringPtr((*this->loopNamesPtr)[L] + ":------------LOOP START--------------\n");
	Value *printfENDstr = builder.CreateGlobalStringPtr((*this->loopNamesPtr)[L] + ":------------LOOP END----------------\n");
	assert(this->loopNamesPtr->find(L) != this->loopNamesPtr->end());
	Value *loopName = builder.CreateGlobalStringPtr((*this->loopNamesPtr)[L]);

	//	Constant* printfMARKER = F.getParent()->getOrInsertFunction(
	//					  "printf",
	//					  FunctionType::getVoidTy(Ctx),
	//					  Type::getInt8PtrTy(Ctx),
	//					  NULL);

	auto loopStartFn = F.getParent()->getOrInsertFunction(
			"loopStart",
			FunctionType::getVoidTy(Ctx),
			morpher::llvm_compat::getInt8PtrTy(Ctx));

	builder.CreateCall(loopStartFn, {loopName});

	//the entry location for instrumentation code
	BasicBlock *loopHeader = L->getHeader();
	BasicBlock::iterator instIter = loopHeader->begin();
	Instruction *loopStartIns = &*instIter;

	SmallVector<BasicBlock *, 8> loopExitBlocksSV;
	L->getExitBlocks(loopExitBlocksSV);

	std::set<BasicBlock *> s;
	for (unsigned i = 0; i < loopExitBlocksSV.size(); ++i)
		s.insert(loopExitBlocksSV[i]);
	std::vector<BasicBlock *> loopExitBlocks;
	loopExitBlocks.assign(s.begin(), s.end());

	std::vector<Instruction *> loopExitInsVec;
	assert(loopExitBlocks.size() != 0);
	for (int i = 0; i < loopExitBlocks.size(); ++i)
	{
		BasicBlock *LoopExitBB = loopExitBlocks[i];
		//		BasicBlock::iterator instIter = --LoopExitBB->end();
		BasicBlock::iterator instIter = LoopExitBB->begin();
		LLVM_DEBUG(dbgs() << "LoopExit Blocks : \n");
		LLVM_DEBUG(LoopExitBB->dump());

		if (std::find(loopExitInsVec.begin(), loopExitInsVec.end(), &*instIter) == loopExitInsVec.end())
		{
			loopExitInsVec.push_back(&*instIter);
		}
	}

	//	IRBuilder<> builder(loopStartIns);
	//	builder.SetInsertPoint(loopStartIns);
	builder.SetInsertPoint(loopHeader, loopHeader->getFirstInsertionPt());
	auto clearPrintedArrs = F.getParent()->getOrInsertFunction(
			"clearPrintedArrs", FunctionType::getVoidTy(Ctx));

	//	for (int i = 0; i < loopExitInsVec.size(); ++i) {
	//		builder.SetInsertPoint(loopExitInsVec[i]);
	//		builder.CreateCall(clearPrintedArrs);
	//	}

	for (int i = 0; i < loopExitBlocks.size(); ++i)
	{
		builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
		builder.CreateCall(clearPrintedArrs);
	}

	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		if (node->getNameType().compare("OutLoopLOAD") == 0)
		{
			if (!node->isTransferedByHost())
			{
				continue;
			}
			LLVM_DEBUG(dbgs() << "OutLoopLoad found!!\n");
			Value *printfstr = builder.CreateGlobalStringPtr("OutLoopLoadNode:%d,val=%d,addr=%d\n");
			assert(OutLoopNodeMapReverse[node] != NULL);
			LLVM_DEBUG(OutLoopNodeMapReverse[node]->dump());
			Value *loadVal = OutLoopNodeMapReverse[node];
			Value *nodeIdx = ConstantInt::get(Type::getInt32Ty(Ctx), node->getIdx());
			Value *addrVal = ConstantInt::get(Type::getInt32Ty(Ctx), node->getoutloopAddr());
			//			Value* args[] = {printfstr,nodeIdx,loadVal,addrVal};

			//			Constant* printf = F.getParent()->getOrInsertFunction(
			//							  "printf",
			//							  FunctionType::getVoidTy(Ctx),
			//							  Type::getInt8PtrTy(Ctx),
			//							  Type::getInt32Ty(Ctx),
			//							  loadVal->getType(),
			//							  Type::getInt32Ty(Ctx),
			//							  NULL);

			auto outloopReportFn = F.getParent()->getOrInsertFunction(
					"outloopValueReport",
					FunctionType::getVoidTy(Ctx),
					Type::getInt32Ty(Ctx), //nodeIdx
					loadVal->getType(),	//value
					Type::getInt32Ty(Ctx), //addr
					Type::getInt8Ty(Ctx),  //isLoad
					Type::getInt8Ty(Ctx),  //isHostTrans
					Type::getInt8Ty(Ctx)   //size
			);

			Value *isLoad = ConstantInt::get(Type::getInt8Ty(Ctx), 1);
			Value *isHostTrans;
			Value *size = ConstantInt::get(Type::getInt8Ty(Ctx), node->getTypeSizeBytes());
			if (node->isTransferedByHost())
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 1);
			}
			else
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 0);
			}
			Value *args[] = {nodeIdx, loadVal, addrVal, isLoad, isHostTrans, size};
			for (int i = 0; i < loopExitBlocks.size(); ++i)
			{
				builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
				//				builder.CreateCall(printf,args);
				builder.CreateCall(outloopReportFn, args);
				//				LLVM_DEBUG(dbgs() << "its a load\n";
				//				loopExitBlocks[i]->dump();
			}
		}

		if (node->getNameType().compare("OutLoopSTORE") == 0)
		{
			if (!node->isTransferedByHost())
			{
				continue;
			}
			Value *printfstr = builder.CreateGlobalStringPtr("OutLoopStoreNode:%d,val=%d,addr=%d\n");
			assert(node->getAncestors().size() == 1);
			Value *StoreVal = (node->getAncestors())[0]->getNode();
			Value *nodeIdx = ConstantInt::get(Type::getInt32Ty(Ctx), node->getIdx());
			Value *addrVal = ConstantInt::get(Type::getInt32Ty(Ctx), node->getoutloopAddr());
			//			Value* args[] = {printfstr,nodeIdx,StoreVal,addrVal};

			//			Constant* printf = F.getParent()->getOrInsertFunction(
			//							  "printf",
			//							  FunctionType::getVoidTy(Ctx),
			//							  Type::getInt8PtrTy(Ctx),
			//							  Type::getInt32Ty(Ctx),
			//							  StoreVal->getType(),
			//							  Type::getInt32Ty(Ctx),
			//							  NULL);

			auto outloopReportFn = F.getParent()->getOrInsertFunction(
					"outloopValueReport",
					FunctionType::getVoidTy(Ctx),
					Type::getInt32Ty(Ctx), //nodeIdx
					StoreVal->getType(),   //value
					Type::getInt32Ty(Ctx), //addr
					Type::getInt8Ty(Ctx),  //isLoad
					Type::getInt8Ty(Ctx),  //isHostTrans
					Type::getInt8Ty(Ctx)   //size
			);

			Value *isLoad = ConstantInt::get(Type::getInt8Ty(Ctx), 0);
			Value *isHostTrans;
			Value *size = ConstantInt::get(Type::getInt8Ty(Ctx), node->getTypeSizeBytes());
			if (node->isTransferedByHost())
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 1);
			}
			else
			{
				isHostTrans = ConstantInt::get(Type::getInt8Ty(Ctx), 0);
			}
			Value *args[] = {nodeIdx, StoreVal, addrVal, isLoad, isHostTrans, size};
			for (int i = 0; i < loopExitBlocks.size(); ++i)
			{
				builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
				//				builder.CreateCall(printf,args);
				builder.CreateCall(outloopReportFn, args);
				//				LLVM_DEBUG(dbgs() << "its a store\n";
				//				OutLoopNodeMapReverse[node]->dump();
				//				loopExitBlocks[i]->dump();
			}
		}

		if (node->getNode() == NULL)
		{
			continue;
		}
		Instruction *ins = node->getNode();

		//		for (auto &B : F){
		//			BasicBlock* BB = dyn_cast<BasicBlock>(&B);
		//			for(auto &I : *BB){
		//
		//			Instruction* ins = &I;

		//			LLVM_DEBUG(dbgs() << "GEPInvestigate node found\n";

		if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(ins))
		{
			const DataLayout DL = ins->getParent()->getParent()->getParent()->getDataLayout();

			//number of elements
			LLVM_DEBUG(dbgs() << "Pointer operand = " << GEP->getPointerOperand()->getName() << "\n");
			LLVM_DEBUG(GEP->dump());
			Type *T = GEP->getSourceElementType();

			LLVM_DEBUG(dbgs() << "S/A/I/P=" << T->isStructTy() << "/");
			LLVM_DEBUG(dbgs() << T->isArrayTy() << "/");
			LLVM_DEBUG(dbgs() << T->isIntegerTy() << "/");
			LLVM_DEBUG(dbgs() << T->isPointerTy() << "\n");

			if (dyn_cast<StructType>(T))
			{

				if (handledGEPs.find(GEP->getPointerOperand()) != handledGEPs.end())
				{
					continue;
				}

				StructType *ST = dyn_cast<StructType>(T);
				LLVM_DEBUG(dbgs() << "StructType=" << ST->getName() << "\n");

				//				//Insert a call to our function
				//				builder(loopStartIns);
				//				builder.SetInsertPoint(loopStartIns);
				builder.SetInsertPoint(loopHeader, loopHeader->getFirstInsertionPt());
				//				builder.SetInsertPoint(loopHeader,++builder.GetInsertPoint());

				Value *old = GEP->getPointerOperand();
				Value *bitcastedPtr = builder.CreateBitCast(old, morpher::llvm_compat::getInt8PtrTy(Ctx));
				int size = DL.getTypeAllocSize(ST);

				(*sizeArrMap)[GEP->getPointerOperand()->getName().str()] = size;

				if (allocatedArraysMap.find(GEP->getPointerOperand()->getName().str()) == allocatedArraysMap.end())
				{
					dfgNode *memopChild = node->getChildren()[0];
					assert(memopChild->getLeftAlignedMemOp() != 0);
					if (memopChild->getLeftAlignedMemOp() == 1)
					{
						node->setGEPbaseAddr(arrayAddrPtrLeft);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrLeft << ",size=" << size << "\n");
						allocatedArraysMap[GEP->getPointerOperand()->getName().str()] = arrayAddrPtrLeft;
						arrayAddrPtrLeft += size;
					}
					else
					{
						node->setGEPbaseAddr(arrayAddrPtrRight);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrRight << ",size=" << size << "\n");
						allocatedArraysMap[GEP->getPointerOperand()->getName().str()] = arrayAddrPtrRight;
						arrayAddrPtrRight += size;
					}
				}
				else
				{
					node->setGEPbaseAddr(allocatedArraysMap[GEP->getPointerOperand()->getName().str()]);
					LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << allocatedArraysMap[GEP->getPointerOperand()->getName().str()] << ",size=" << size << "\n");
				}

				auto printArrFunc = F.getParent()->getOrInsertFunction(
						"printArr",
						FunctionType::getVoidTy(Ctx),
						morpher::llvm_compat::getInt8PtrTy(Ctx),
						morpher::llvm_compat::getInt8PtrTy(Ctx),
						Type::getInt32Ty(Ctx),
						Type::getInt8Ty(Ctx),
						Type::getInt32Ty(Ctx));

				Value *st_name = builder.CreateGlobalStringPtr(ST->getName());
				Value *argsi[] = {st_name,
						bitcastedPtr,
						ConstantInt::get(Type::getInt32Ty(Ctx), size),
						ConstantInt::get(Type::getInt8Ty(Ctx), 1),
						ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
				Value *argso[] = {st_name,
						bitcastedPtr,
						ConstantInt::get(Type::getInt32Ty(Ctx), size),
						ConstantInt::get(Type::getInt8Ty(Ctx), 0),
						ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
				//				builder.CreateCall(printArrFunc,args);

				builder.CreateCall(printArrFunc, argsi);

				//				for (int i = 0; i < loopExitInsVec.size(); ++i) {
				//					builder.SetInsertPoint(loopExitInsVec[i]);
				//					builder.CreateCall(printArrFunc,argso);
				//				}

				for (int i = 0; i < loopExitBlocks.size(); ++i)
				{
					builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
					builder.CreateCall(printArrFunc, argso);
				}

				//experiment
				//				LLVM_DEBUG(dbgs() << "Experiment, arraytype size = " << AT->getArrayNumElements() << "\n";
			}
			else
			{
				if (dyn_cast<ArrayType>(T))
				{

					if (handledGEPs.find(GEP->getPointerOperand()) != handledGEPs.end())
					{
						continue;
					}

					ArrayType *AT = dyn_cast<ArrayType>(T);
					LLVM_DEBUG(dbgs() << "ArrayType=" << AT->getArrayNumElements() << "\n");
					LLVM_DEBUG(dbgs() << "Size = " << DL.getTypeAllocSize(AT) << "\n");

					//					//Insert a call to our function
					//					IRBuilder<> builder(loopStartIns);
					//					builder.SetInsertPoint(loopStartIns);
					builder.SetInsertPoint(loopHeader, loopHeader->getFirstInsertionPt());

					Value *old = GEP->getPointerOperand();
					Value *bitcastedPtr = builder.CreateBitCast(old, morpher::llvm_compat::getInt8PtrTy(Ctx));
					int size = DL.getTypeAllocSize(AT);

					(*sizeArrMap)[GEP->getPointerOperand()->getName().str()] = size;
					dfgNode *memopChild = node->getChildren()[0];
					assert(memopChild->getLeftAlignedMemOp() != 0);
					if (memopChild->getLeftAlignedMemOp() == 1)
					{
						node->setGEPbaseAddr(arrayAddrPtrLeft);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrLeft << ",size=" << size << "\n");
						arrayAddrPtrLeft += size;
					}
					else
					{
						node->setGEPbaseAddr(arrayAddrPtrRight);
						LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrRight << ",size=" << size << "\n");
						arrayAddrPtrRight += size;
					}

					auto printArrFunc = F.getParent()->getOrInsertFunction(
							"printArr",
							FunctionType::getVoidTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							Type::getInt32Ty(Ctx),
							Type::getInt8Ty(Ctx),
							Type::getInt32Ty(Ctx));

					Value *st_name = builder.CreateGlobalStringPtr(GEP->getPointerOperand()->getName());
					Value *argsi[] = {st_name,
							bitcastedPtr,
							ConstantInt::get(Type::getInt32Ty(Ctx), size),
							ConstantInt::get(Type::getInt8Ty(Ctx), 1),
							ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
					Value *argso[] = {st_name,
							bitcastedPtr,
							ConstantInt::get(Type::getInt32Ty(Ctx), size),
							ConstantInt::get(Type::getInt8Ty(Ctx), 0),
							ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};

					//Add a call in the begginning of the loop
					builder.CreateCall(printArrFunc, argsi);

					//					LLVM_DEBUG(dbgs() << "loopExitInsVec.size()=" << loopExitInsVec.size() << "\n";
					//					for (int i = 0; i < loopExitInsVec.size(); ++i) {
					//						loopExitInsVec[i]->dump();
					//						builder.SetInsertPoint(loopExitInsVec[i]);
					//						builder.CreateCall(printArrFunc,argso);
					//					}

					for (int i = 0; i < loopExitBlocks.size(); ++i)
					{
						builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
						builder.CreateCall(printArrFunc, argso);
					}
				}
				else
				{
					assert(dyn_cast<IntegerType>(T));
					IntegerType *IT = dyn_cast<IntegerType>(T);

					//					IRBuilder<> builder(GEP);
					builder.SetInsertPoint(GEP);
					//					builder.SetInsertPoint(loopHeader,++builder.GetInsertPoint());

					Value *old = GEP->getPointerOperand();
					Value *addr = GEP;
					Value *bitcastedPtr = builder.CreateBitCast(old, morpher::llvm_compat::getInt8PtrTy(Ctx));
					int size = DL.getTypeAllocSize(IT);

					auto reportDynArrSize = F.getParent()->getOrInsertFunction(
							"reportDynArrSize", FunctionType::getVoidTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							morpher::llvm_compat::getInt8PtrTy(Ctx),
							Type::getInt32Ty(Ctx),
							Type::getInt32Ty(Ctx));

					Value *st_name = builder.CreateGlobalStringPtr(GEP->getPointerOperand()->getName());
					Value *args[] = {st_name,
							bitcastedPtr,
							GEP->getOperand(1),
							ConstantInt::get(Type::getInt32Ty(Ctx),
									size)};
					builder.CreateCall(reportDynArrSize, args);

					std::string ptrName = GEP->getPointerOperand()->getName().str();

					//checking for user defined input sizes
					if (sizeArrMap->find(ptrName) != sizeArrMap->end())
					{
						//						builder.SetInsertPoint(loopStartIns);
						//						builder.SetInsertPoint(loopHeader,++builder.GetInsertPoint());
						int size = (*sizeArrMap)[ptrName];

						if (allocatedArraysMap.find(ptrName) == allocatedArraysMap.end())
						{
							dfgNode *memopChild = node->getChildren()[0];
							assert(memopChild->getLeftAlignedMemOp() != 0);
							if (memopChild->getLeftAlignedMemOp() == 1)
							{
								node->setGEPbaseAddr(arrayAddrPtrLeft);
								LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrLeft << ",size=" << size << "\n");
								allocatedArraysMap[ptrName] = arrayAddrPtrLeft;
								arrayAddrPtrLeft += size;
							}
							else
							{
								node->setGEPbaseAddr(arrayAddrPtrRight);
								LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << arrayAddrPtrRight << ",size=" << size << "\n");
								allocatedArraysMap[ptrName] = arrayAddrPtrRight;
								arrayAddrPtrRight += size;
							}
						}
						else
						{
							node->setGEPbaseAddr(allocatedArraysMap[ptrName]);
							LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",addr=" << allocatedArraysMap[ptrName] << ",size=" << size << "\n");
						}

						auto printArrFunc = F.getParent()->getOrInsertFunction(
								"printArr",
								FunctionType::getVoidTy(Ctx),
								morpher::llvm_compat::getInt8PtrTy(Ctx),
								morpher::llvm_compat::getInt8PtrTy(Ctx),
								Type::getInt32Ty(Ctx),
								Type::getInt8Ty(Ctx),
								Type::getInt32Ty(Ctx));

						Value *argsi[] = {st_name,
								bitcastedPtr,
								ConstantInt::get(Type::getInt32Ty(Ctx), size),
								ConstantInt::get(Type::getInt8Ty(Ctx), 1),
								ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
						Value *argso[] = {st_name,
								bitcastedPtr,
								ConstantInt::get(Type::getInt32Ty(Ctx), size),
								ConstantInt::get(Type::getInt8Ty(Ctx), 0),
								ConstantInt::get(Type::getInt32Ty(Ctx), node->getGEPbaseAddr())};
						//Add a call in the begginning of the loop
						builder.CreateCall(printArrFunc, argsi);
						//
						//						LLVM_DEBUG(dbgs() << "loopExitInsVec.size()=" << loopExitInsVec.size() << "\n";
						//						for (int i = 0; i < loopExitInsVec.size(); ++i) {
						//							loopExitInsVec[i]->dump();
						//							builder.SetInsertPoint(loopExitInsVec[i]);
						//							builder.CreateCall(printArrFunc,argso);
						//						}

						for (int i = 0; i < loopExitBlocks.size(); ++i)
						{
							builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
							builder.CreateCall(printArrFunc, argso);
						}
					}
					else
					{
						LLVM_DEBUG(dbgs() << "Please provide sizes for the arrayptr : " << ptrName << "\n");
						assert(0);
					}
				}
			}

			LLVM_DEBUG(dbgs() << "GEPInvestigate : instrument code added!\n");
			handledGEPs.insert(GEP->getPointerOperand());
		}

		//		} // for(auto &I : *BB){
		//	} //for (auto &B : F){

	} //Nodelist

	auto printDynArrSize = F.getParent()->getOrInsertFunction(
			"printDynArrSize", FunctionType::getVoidTy(Ctx));

	auto loopEndFn = F.getParent()->getOrInsertFunction(
			"loopEnd", FunctionType::getVoidTy(Ctx), morpher::llvm_compat::getInt8PtrTy(Ctx));

	//	for (int i = 0; i < loopExitInsVec.size(); ++i) {
	//		builder.SetInsertPoint(loopExitInsVec[i]);
	//		builder.CreateCall(clearPrintedArrs);
	//		builder.CreateCall(loopEndFn,{loopName});
	//	}

	for (int i = 0; i < loopExitBlocks.size(); ++i)
	{
		builder.SetInsertPoint(loopExitBlocks[i], --loopExitBlocks[i]->end());
		builder.CreateCall(clearPrintedArrs);
		builder.CreateCall(loopEndFn, {loopName});
	}

} //End of GEPInvestigate Function

void DFG::AssignOutLoopAddr()
{

	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if ((node->getNameType().compare("OutLoopLOAD") == 0) ||
				(node->getNameType().compare("OutLoopSTORE") == 0))
		{

			assert(node->getMappedLoc() != NULL);

			int bytewidth;
			if (dyn_cast<IntegerType>(OutLoopNodeMapReverse[node]->getType()))
			{
				bytewidth = OutLoopNodeMapReverse[node]->getType()->getIntegerBitWidth() / 8;
			}
			else if (dyn_cast<ArrayType>(OutLoopNodeMapReverse[node]->getType()) ||
					dyn_cast<StructType>(OutLoopNodeMapReverse[node]->getType()) ||
					dyn_cast<PointerType>(OutLoopNodeMapReverse[node]->getType()))
			{
				bytewidth = 4;
			}
			else
			{
				assert(0 && "should not come here");
			}

			if (node->getMappedLoc()->getY() <= 1)
			{
				outloopAddrPtrLeft = outloopAddrPtrLeft - bytewidth;

				if (bytewidth == 4)
				{
					outloopAddrPtrLeft = outloopAddrPtrLeft & (~0b11);
				}
				else if (bytewidth == 2)
				{
					outloopAddrPtrLeft = outloopAddrPtrLeft & (~0b1);
				}

				node->setoutloopAddr(outloopAddrPtrLeft);
				LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",bytewidth=" << bytewidth << ",addr=" << outloopAddrPtrLeft << "\n");
			}
			else if (node->getMappedLoc()->getY() >= 2)
			{
				outloopAddrPtrRight = outloopAddrPtrRight - bytewidth;

				if (bytewidth == 4)
				{
					outloopAddrPtrRight = outloopAddrPtrRight & (~0b11);
				}
				else if (bytewidth == 2)
				{
					outloopAddrPtrRight = outloopAddrPtrRight & (~0b1);
				}

				node->setoutloopAddr(outloopAddrPtrRight);
				LLVM_DEBUG(dbgs() << "NodeIdx:" << node->getIdx() << ",bytewidth=" << bytewidth << ",addr=" << outloopAddrPtrRight << "\n");
			}
			else
			{
				assert(false);
			}
		}
	}
}

int DFG::phiselectInsert()
{
	return 0;
}

int DFG::PlaceMacro(DFG *mappedDFG, int XDim, int YDim, ArchType arch)
{
	if (this->currCGRA != NULL)
	{
		assert(this->currCGRA->getMapped() == false);
	}
	int II = 0; //this will be one inititally once it gets into the following while loop
	bool macroPlaceSuccess = false;

	while (macroPlaceSuccess == false)
	{
		II++;
		delete currCGRA;
		currCGRA = new CGRA(II, XDim, YDim, REGS_PER_NODE, arch);
		macroPlaceSuccess = currCGRA->PlaceMacro(mappedDFG);
	}

	return II;
}

int DFG::classifyParents()
{
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		LLVM_DEBUG(dbgs() << "classifyParent::currentNode = " << node->getIdx() << "\n");
		LLVM_DEBUG(dbgs() << "Parents : ");
		for (dfgNode *parent : node->getAncestors())
		{
			LLVM_DEBUG(dbgs() << parent->getIdx() << ",");
		}
		LLVM_DEBUG(dbgs() << "\n");

		for (dfgNode *parent : node->getAncestors())
		{
			Instruction *ins;
			Value *parentIns;

			if (node->getNode() != NULL)
			{
				ins = node->getNode();
			}
			else
			{
				if (node->getNameType().compare("XORNOT") == 0)
				{
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1] = parent;
					continue;
				}
				if (node->getNameType().compare("ORZERO") == 0)
				{
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1] = parent;
					continue;
				}
					if (node->getNameType().compare("GEPLEFTSHIFT") == 0
						|| node->getNameType().compare("GEPSCALE") == 0)
					{
						if (parent->getNode() != NULL)
						{
							parentIns = parent->getNode();
						if (BranchInst *BRI = dyn_cast<BranchInst>(parentIns))
						{
							node->parentClassification[0] = parent;
							continue;
						}
						else if (CmpInst *CMPI = dyn_cast<CmpInst>(parentIns))
						{
							node->parentClassification[0] = parent;
							continue;
						}
					}
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1] = parent;
					continue;
				}
				if (node->getNameType().compare("MASKAND") == 0)
				{
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1] = parent;
					continue;
				}
				if (node->getNameType().compare("PREDAND") == 0)
				{
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1] = parent;
					continue;
				}
				if (node->getNameType().compare("CTRLBrOR") == 0)
				{
					if (node->parentClassification.find(1) == node->parentClassification.end())
					{
						node->parentClassification[1] = parent;
					}
					else
					{
						assert(node->parentClassification.find(2) == node->parentClassification.end());
						node->parentClassification[2] = parent;
					}
					continue;
				}
				if (node->getNameType().compare("GEPADD") == 0)
				{
					if (node->parentClassification.find(1) == node->parentClassification.end())
					{
						node->parentClassification[1] = parent;
					}
					else
					{
						assert(node->parentClassification.find(2) == node->parentClassification.end());
						node->parentClassification[2] = parent;
					}
					continue;
				}
				if (node->getNameType().compare("STORESTART") == 0)
				{
					assert(parent->getNode() == NULL);
					if (parent->getNameType().compare("MOVC") == 0)
					{
						node->parentClassification[1] = parent;
					}
					else if (parent->getNameType().compare("LOOPSTART") == 0)
					{
						node->parentClassification[0] = parent;
					}
					else if (parent->getNameType().compare("PREDAND") == 0)
					{
						node->parentClassification[0] = parent;
					}
					else
					{
						assert(false);
					}
					continue;
				}
				if (node->getNameType().compare("LOOPEXIT") == 0)
				{
					if (parent->getNode() == NULL)
					{
						if (parent->getNameType().compare("MOVC") == 0)
						{
							assert(node->parentClassification.find(1) == node->parentClassification.end());
							node->parentClassification[1] = parent;
						}
						else if (parent->getNameType().compare("CTRLBrOR") == 0)
						{
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0] = parent;
						}
						else
						{
							assert(false);
						}
					}
					else
					{
						assert(node->parentClassification.find(0) == node->parentClassification.end());
						node->parentClassification[0] = parent;
					}
					continue;
				}
				if (node->getNameType().compare("OutLoopLOAD") == 0)
				{
					//do nothing
					continue;
				}
				if (node->getNameType().compare("OutLoopSTORE") == 0)
				{
					assert(node->hasConstantVal());

					//check for predicate parent
					if (parent->getNode() != NULL)
					{
						parentIns = parent->getNode();
						if (BranchInst *BRI = dyn_cast<BranchInst>(parentIns))
						{
							node->parentClassification[0] = parent;
							continue;
						}
						else if (CmpInst *CMPI = dyn_cast<CmpInst>(parentIns))
						{
							node->parentClassification[0] = parent;
							continue;
						}
					}
					else
					{
						if (parent->getNameType().compare("CTRLBrOR") == 0)
						{
							node->parentClassification[0] = parent;
							continue;
						}
					}

					//if it comes to this, it has to be a data parent.
					node->parentClassification[1] = parent;
					continue;
				}
				if (node->getNameType().compare("CMERGE") == 0)
				{

					Value *parentIns = NULL;
					if (parent->getNameType().compare("OutLoopLOAD") == 0)
					{
						parentIns = OutLoopNodeMapReverse[parent];
					}
					else
					{
						parentIns = parent->getNode();
					}

					assert(node->getChildren().size() == 1);
					dfgNode *cmergeChild = node->getChildren()[0];
					assert(cmergeChild->getNode() != NULL);
					Instruction *condIns;
					if (SelectInst *SLI = dyn_cast<SelectInst>(cmergeChild->getNode()))
					{
						condIns = dyn_cast<Instruction>(SLI->getCondition());
						assert(condIns);
					}

					if (parentIns != NULL)
					{
						if ((dyn_cast<BranchInst>(parentIns)) || (dyn_cast<CmpInst>(parentIns)))
						{
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0] = parent;
						}
						else if (parentIns == condIns)
						{
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0] = parent;
						}
						else
						{
							if (node->parentClassification.find(1) != node->parentClassification.end())
							{
								LLVM_DEBUG(parentIns->dump());
							}
							assert(node->parentClassification.find(1) == node->parentClassification.end());
							node->parentClassification[1] = parent;
						}
					}
					else
					{
						if ((parent->getNameType().compare("XORNOT") == 0))
						{
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0] = parent;
						}
						else if ((parent->getNameType().compare("ORZERO") == 0))
						{
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0] = parent;
						}
						else if (parent->getNameType().compare("LOOPSTART") == 0)
						{
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0] = parent;
							node->setNPB(true);
						}
						else if (parent->getNameType().compare("PREDAND") == 0)
						{
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0] = parent;
							node->setNPB(true);
						}
						else
						{
							LLVM_DEBUG(dbgs() << "Parent :" << parent->getIdx() << ", classified as I1\n");
							assert(node->parentClassification.find(1) == node->parentClassification.end());
							node->parentClassification[1] = parent;
						}
					}

					//						if(node->parentClassification.find(1) == node->parentClassification.end()){
					//							node->parentClassification[1]=parent;
					//						}
					//						else{
					//							assert(node->parentClassification.find(2) == node->parentClassification.end());
					//							node->parentClassification[2]=parent;
					//						}
					continue;
				}
			}

			if (dyn_cast<PHINode>(ins) || dyn_cast<SelectInst>(ins))
			{
				if (parent->getNode() != NULL)
					LLVM_DEBUG(parent->getNode()->dump());
				LLVM_DEBUG(dbgs() << "node : " << node->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "Parent : " << parent->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "Parent NameType : " << parent->getNameType() << "\n");

				if (parent->getNode())
				{
					if (dyn_cast<BranchInst>(parent->getNode()))
					{
						node->parentClassification[0] = parent;
						continue;
					}
				}

				assert(parent->getNameType().compare("CMERGE") == 0);
				if (node->parentClassification.find(1) == node->parentClassification.end())
				{
					node->parentClassification[1] = parent;
				}
				else if (node->parentClassification.find(2) == node->parentClassification.end())
				{
					//						assert(node->parentClassification.find(2) == node->parentClassification.end());
					node->parentClassification[2] = parent;
				}
				else
				{
					LLVM_DEBUG(dbgs() << "Extra parent : " << parent->getIdx() << ",Nametype = " << parent->getNameType() << ",par_idx = " << node->parentClassification.size() + 1 << "\n");
					node->parentClassification[node->parentClassification.size() + 1] = parent;
				}
				continue;
			}

			if (dyn_cast<BranchInst>(ins))
			{
				if (node->parentClassification.find(1) == node->parentClassification.end())
				{
					node->parentClassification[1] = parent;
				}
				else
				{
					assert(node->parentClassification.find(2) == node->parentClassification.end());
					node->parentClassification[2] = parent;
				}
				continue;
			}

			if (parent->getNode() != NULL)
			{
				parentIns = parent->getNode();
				if (BranchInst *BRI = dyn_cast<BranchInst>(parentIns))
				{
					node->parentClassification[0] = parent;
					continue;
				}
				//					else if(CmpInst * CMPI = dyn_cast<CmpInst>(parentIns)){
				//						node->parentClassification[0]=parent;
				//						continue;
				//					}
			}
			else
			{
				if (parent->getNameType().compare("CTRLBrOR") == 0)
				{
					node->parentClassification[0] = parent;
					continue;
				}
				if (parent->getNameType().compare("PREDAND") == 0)
				{
					node->parentClassification[0] = parent;
					continue;
				}
				//					if(parent->getNameType().compare("GEPLEFTSHIFT")==0){
				//						assert(node->parentClassification.find(1)==node->parentClassification.end());
				//						node->parentClassification[1]=parent;
				//						continue;
				//					}
					if (parent->getNameType().compare("GEPLEFTSHIFT") == 0
						|| parent->getNameType().compare("GEPSCALE") == 0)
					{
						assert(parent->getAncestors().size() < 3);
						node->parentClassification[1] = parent;
						continue;
				}
				if (parent->getNameType().compare("MASKAND") == 0)
				{
					assert(parent->getAncestors().size() == 1);
					parentIns = parent->getAncestors()[0]->getNode();
				}
				if (parent->getNameType().compare("OutLoopLOAD") == 0)
				{
					parentIns = OutLoopNodeMapReverse[parent];
				}
				if (parent->getNameType().compare("CMERGE") == 0)
				{
					if (node->parentClassification.find(1) == node->parentClassification.end())
					{
						node->parentClassification[1] = parent;
					}
					else if (node->parentClassification.find(2) == node->parentClassification.end())
					{
						//						assert(node->parentClassification.find(2) == node->parentClassification.end());
						node->parentClassification[2] = parent;
					}
					else
					{
						LLVM_DEBUG(dbgs() << "Extra parent : " << parent->getIdx() << ",Nametype = " << parent->getNameType() << ",par_idx = " << node->parentClassification.size() + 1 << "\n");
						node->parentClassification[node->parentClassification.size() + 1] = parent;
					}
					continue;
				}
			}
			//Only ins!=NULL and non-PHI and non-BR will reach here
			node->parentClassification[findOperandNumber(node, ins, parentIns)] = parent;
		}
	}
	return 0;
}

int DFG::findOperandNumber(dfgNode *node, Instruction *child, Value *parent)
{
	//	parent->dump();
	//	child->dump();

	if (LoadInst *LDI = dyn_cast<LoadInst>(child))
	{
		assert(parent == LDI->getPointerOperand());
		return 2;
	}
	else if (StoreInst *STI = dyn_cast<StoreInst>(child))
	{
		if (parent == STI->getPointerOperand())
		{
			return 2;
		}
		else if (parent == STI->getValueOperand())
		{
			return 1;
		}
		else
		{
			return 0;
		}
	}
	else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(child))
	{
		if (node->getAncestors().size() > 1)
		{
			LLVM_DEBUG(dbgs() << "node :");
			LLVM_DEBUG(child->dump());
			for (dfgNode *parentNode : node->getAncestors())
			{
				if (parentNode->getNode())
				{
					LLVM_DEBUG(dbgs() << "parent :");
					LLVM_DEBUG(parentNode->getNode()->dump());
				}
				else
				{
					LLVM_DEBUG(dbgs() << "parent :" << parentNode->getDFSIdx() << "," << parentNode->getNameType() << "\n");
				}
			}

			for (int i = 0; i < child->getNumOperands(); ++i)
			{
				LLVM_DEBUG(child->getOperand(i)->dump());
				if (child->getOperand(i) == parent)
				{
					return i + 1;
				}
			}
		}
		//TODO : please remove this
		assert(node->getAncestors().size() == 1);
		return 1;
	}
	else
	{
		if (child->getNumOperands() == 3)
		{
			for (int i = 0; i < 3; ++i)
			{
				if (Value *chkIns = (child->getOperand(i)))
				{
					if (parent == chkIns)
					{
						return i;
					}
				}
				else
				{
					//					LLVM_DEBUG(dbgs() << "child :\n";
					LLVM_DEBUG(child->dump());
					//					LLVM_DEBUG(dbgs() << "parent :\n";
					LLVM_DEBUG(parent->dump());
					//					LLVM_DEBUG(dbgs() << child->getOperand(i)->getName() << "\n";
					assert(i != 0);
					ConstantInt *CI = cast<ConstantInt>(child->getOperand(i));
					if (i == 1)
					{
						assert(parent == (child->getOperand(2)));
						return 2;
					}
					else
					{ // i==2
						assert(parent == (child->getOperand(1)));
						return 1;
					}
				}
			}
		}
		else if (child->getNumOperands() == 2)
		{
			for (int i = 0; i < 2; ++i)
			{
				if (Value *chkIns = (child->getOperand(i)))
				{
					if (parent == chkIns)
					{
						return i + 1;
					}
				}
				else
				{
					//					ConstantInt* CI = cast<ConstantInt>(child->getOperand(i));
					//					assert(node->hasConstantVal());
					if (i == 0)
					{
						assert(parent == (child->getOperand(1)));
						return 1;
					}
					else
					{ // i==1
						assert(parent == (child->getOperand(0)));
						return 0;
					}
				}
			}
		}
		else if (child->getNumOperands() == 1)
		{
			assert(parent == (child->getOperand(0)));
			return 1;
		}
		else
		{
			assert(false);
		}
	}
	return 0;
}

int DFG::treatFalsePaths()
{
	dfgNode *node;
	int NOTsadded = 0;
	for (int i = 0; i < NodeList.size(); i++)
	{
		node = NodeList[i];
		for (dfgNode *parent : node->getAncestors())
		{
			if (parent->getNode() != NULL)
			{
				if (BranchInst *BRI = dyn_cast<BranchInst>(parent->getNode()))
				{
					if (node->getNameType().compare("CTRLBrOR") == 0)
					{
						if (BRI->isConditional())
						{
							if (node->BB == BRI->getSuccessor(1))
							{
								parent->removeChild(node);
								node->removeAncestor(parent);
								removeEdge(findEdge(parent, node));

								dfgNode *temp = new dfgNode(this);
								temp->setNameType("XORNOT");
								temp->setIdx(NodeList.size());
								temp->BB = node->BB;
								NodeList.push_back(temp);

								parent->addChildNode(temp);
								temp->addAncestorNode(parent);

								temp->addChildNode(node);
								node->addAncestorNode(temp);
								NOTsadded++;
							}
						}
					}
					else
					{
						if (BRI->isConditional())
						{
							if (node->BB == BRI->getSuccessor(1))
							{
								node->setNPB(true);
							}
						}
					}
				}
			}
		}
	}
	LLVM_DEBUG(dbgs() << "treatFalsePaths:: NOTsadded = " << NOTsadded << "\n");
	return 0;
}

int DFG::insertshiftGEPs()
{
	LLVM_DEBUG(dbgs() << "insertshiftGEPs started!\n");
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		if (node->getNode() != NULL)
		{
			if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(node->getNode()))
			{

				int elementSize = 0;
				LLVM_DEBUG(GEP->dump());
				for (dfgNode *child : node->getChildren())
				{
					if (child->getNode() != NULL)
					{
						if (child->getNode()->mayReadOrWriteMemory())
						{
							elementSize = child->getTypeSizeBytes() / 4;
							LLVM_DEBUG(dbgs() << "GEP element size = " << child->getTypeSizeBytes() / 4 << "\n");
							break;
						}
					}
				}

				std::set<dfgNode *> non_control_parents;
				for (dfgNode *parent : node->getAncestors())
				{
					if (parent->getNameType() == "CTRLBrOR")
					{
						continue;
					}
					else if (parent->getNode() != NULL)
					{
						Instruction *parentIns = parent->getNode();
						if (BranchInst *BRI = dyn_cast<BranchInst>(parentIns))
						{
							continue;
						}
						else if (CmpInst *CMPI = dyn_cast<CmpInst>(parentIns))
						{
							continue;
						}
					}
					non_control_parents.insert(parent);
				}
				assert(non_control_parents.size() <= 2);

				if (non_control_parents.size() == 2)
				{
					dfgNode *tempADD = new dfgNode(this);
					tempADD->setNameType("GEPADD");
					tempADD->setIdx(NodeList.size());
					NodeList.push_back(tempADD);
					tempADD->BB = node->BB;

					for (dfgNode *parent : non_control_parents)
					{
						parent->removeChild(node);
						node->removeAncestor(parent);
						removeEdge(findEdge(parent, node));

						parent->addChildNode(tempADD);
						tempADD->addAncestorNode(parent);
					}

						if (elementSize > 1)
						{
							dfgNode *temp = new dfgNode(this);
							temp->setIdx(NodeList.size());
							NodeList.push_back(temp);
							configureGEPScaleNode(temp, elementSize);
							temp->BB = node->BB;

						tempADD->addChildNode(temp);
						temp->addAncestorNode(tempADD);

						temp->addChildNode(node);
						node->addAncestorNode(temp);
					}
				}
				else
				{
					assert(non_control_parents.size() == 1);
						if (elementSize > 1)
						{
							dfgNode *temp = new dfgNode(this);
							temp->setIdx(NodeList.size());
							NodeList.push_back(temp);
							configureGEPScaleNode(temp, elementSize);
							temp->BB = node->BB;

						for (dfgNode *parent : node->getAncestors())
						{
							parent->removeChild(node);
							node->removeAncestor(parent);
							removeEdge(findEdge(parent, node));

							parent->addChildNode(temp);
							temp->addAncestorNode(parent);
						}
						temp->addChildNode(node);
						node->addAncestorNode(temp);
					}
				}
			}
		}
	}
	LLVM_DEBUG(dbgs() << "insertshiftGEPs ended!\n");
	return 0;
}

int DFG::partitionMemNodes()
{
	dfgNode *node;
	std::map<Value *, std::vector<dfgNode *>> pointerMemInsMap;
	std::vector<Value *> pointerOperandVec;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getNode() != NULL)
		{
			if (LoadInst *LDI = dyn_cast<LoadInst>(node->getNode()))
			{

				if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(LDI->getPointerOperand()))
				{
					//					GetElementPtrInst* GEP = cast<GetElementPtrInst>(LDI->getPointerOperand());
					pointerMemInsMap[GEP->getPointerOperand()].push_back(node);
				}
				else
				{
					pointerMemInsMap[LDI->getPointerOperand()].push_back(node);
				}
			}

			if (StoreInst *STI = dyn_cast<StoreInst>(node->getNode()))
			{

				if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(STI->getPointerOperand()))
				{
					//					GetElementPtrInst* GEP = cast<GetElementPtrInst>(STI->getPointerOperand());
					pointerMemInsMap[GEP->getPointerOperand()].push_back(node);
				}
				else
				{
					pointerMemInsMap[STI->getPointerOperand()].push_back(node);
				}
			}
		}
	}

	int sum = 0;
	for (std::pair<Value *, std::vector<dfgNode *>> pair : pointerMemInsMap)
	{
		sum += pair.second.size();
		pointerOperandVec.push_back(pair.first);
	}

	int n = pointerOperandVec.size();

	bool dp[n + 1][sum + 1];
	std::map<int, std::map<int, std::vector<Value *>>> dpValueMap;

	//first column is true :: 0 sum is possible with all elements
	for (int i = 0; i <= n; ++i)
	{
		dp[i][0] = true;
	}

	// Initialize top row, except dp[0][0], as false. With
	// 0 elements, no other sum except 0 is possible
	for (int j = 1; j <= sum; j++)
	{
		dp[0][j] = false;
	}

	// Fill the partition table in bottom up manner
	for (int i = 1; i <= n; i++)
	{
		for (int j = 1; j <= sum; j++)
		{
			// If i'th element is excluded
			dp[i][j] = dp[i - 1][j];
			if (dp[i - 1][j])
			{
				dpValueMap[i][j] = dpValueMap[i - 1][j];
			}

			// If i'th element is included
			int ithElement = pointerMemInsMap[pointerOperandVec[i - 1]].size();
			if (ithElement <= j)
			{
				dp[i][j] |= dp[i - 1][j - ithElement];
				if (dp[i - 1][j - ithElement])
				{
					dpValueMap[i][j] = dpValueMap[i - 1][j - ithElement];
					dpValueMap[i][j].push_back(pointerOperandVec[i - 1]);
				}
			}
		}
	}

	// Initialize difference of two sums.
	int diff = INT_MAX;

	// Find the largest j such that dp[n][j]
	// is true where j loops from sum/2 t0 0
	int minsumpart = 0;
	for (int j = sum / 2; j >= 0; j--)
	{
		// Find the
		if (dp[n][j] == true)
		{
			diff = sum - 2 * j;
			minsumpart = j;
			break;
		}
	}

	LLVM_DEBUG(dbgs() << "Left side pointers : \n");
	for (Value *pointerOp : dpValueMap[n][minsumpart])
	{
		LLVM_DEBUG(dbgs() << "(" << pointerOp->getName() << "," << pointerMemInsMap[pointerOp].size() << ");");
		for (dfgNode *node : pointerMemInsMap[pointerOp])
		{
			node->setLeftAlignedMemOp(1);
		}
		pointerOperandVec.erase(std::remove(pointerOperandVec.begin(), pointerOperandVec.end(), pointerOp), pointerOperandVec.end());
	}
	LLVM_DEBUG(dbgs() << "\n");

	LLVM_DEBUG(dbgs() << "Right side pointers : \n");
	for (Value *pointerOp : pointerOperandVec)
	{
		LLVM_DEBUG(dbgs() << "(" << pointerOp->getName() << "," << pointerMemInsMap[pointerOp].size() << ");");
		for (dfgNode *node : pointerMemInsMap[pointerOp])
		{
			node->setLeftAlignedMemOp(2);
		}
	}
	LLVM_DEBUG(dbgs() << "\n");

	return 0;
}

int DFG::handlestartstop()
{
	dfgNode *node;

	// Loop *L = this->getLoop();
	// SmallVector<BasicBlock *, 8> loopExitBlocks;
	// L->getExitBlocks(loopExitBlocks);

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getNode() == NULL)
		{
			if (node->getNameType().compare("LOOPSTART") == 0)
			{
				//				assert(false);
			}
		}
	}

	bool startFound = false;
	bool exitFound = false;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		if (startFound & exitFound)
		{
			break;
		}

		//LoopStart
		if (node->getNode() == NULL)
		{
			if (node->getNameType().compare("LOOPSTART") == 0)
			{
				startFound = true;
				//				dfgNode* loadStart = new dfgNode(this);
				//				loadStart->setIdx(this->getNodesPtr()->size());
				//				this->getNodesPtr()->push_back(loadStart);
				//				loadStart->setNameType("LOADSTART");
				//				loadStart->BB = node->BB;

				node->setLeftAlignedMemOp(2);

				dfgNode *storeStart = new dfgNode(this);
				storeStart->setIdx(this->getNodesPtr()->size());
				this->getNodesPtr()->push_back(storeStart);
				storeStart->setNameType("STORESTART");
				storeStart->BB = node->BB;
				storeStart->setLeftAlignedMemOp(2);
				storeStart->setNPB(false);

				node->addChildNode(storeStart);
				storeStart->addAncestorNode(node);

				dfgNode *movone = new dfgNode(this);
				movone->setIdx(this->getNodesPtr()->size());
				this->getNodesPtr()->push_back(movone);
				movone->setNameType("MOVC");
				movone->BB = node->BB;
				movone->setConstantVal(0);

				movone->addChildNode(storeStart);
				storeStart->addAncestorNode(movone);

				//				for (dfgNode* child : node->getChildren()) {
				//					node->removeChild(child);
				//					child->removeAncestor(node);
				//					removeEdge(findEdge(node,child));
				//
				//					loadStart->addChildNode(child);
				//					child->addAncestorNode(loadStart);
				//				}
			}
		}
		else
		{ //LoopExit
			//			if(std::find(loopExitBlocks.begin(),loopExitBlocks.end(),node->getNode()->getParent())!=loopExitBlocks.end()){
			//
			//				if(!dyn_cast<BranchInst>(node->getNode())){
			//					continue;
			//				}
			//
			//				//current node belong to a loop exit basic block
			//				dfgNode* loopexit = new dfgNode(this);
			//				loopexit->setIdx(this->getNodesPtr()->size());
			//				this->getNodesPtr()->push_back(loopexit);
			//				loopexit->setNameType("LOOPEXIT");
			//				loopexit->BB = node->BB;
			//				loopexit->setLeftAlignedMemOp(1);
			//
			//				assert(node->getChildren().empty());
			//				for(dfgNode* anc : node->getAncestors()){
			//					anc->removeChild(node);
			//					node->removeAncestor(anc);
			//					removeEdge(findEdge(anc,node));
			//
			//					anc->addChildNode(loopexit);
			//					loopexit->addAncestorNode(anc);
			//				}
			//
			//				dfgNode* movone = new dfgNode(this);
			//				movone->setIdx(this->getNodesPtr()->size());
			//				this->getNodesPtr()->push_back(movone);
			//				movone->setNameType("MOVC");
			//				movone->BB = node->BB;
			//				movone->setConstantVal(1);
			//
			//				movone->addChildNode(loopexit);
			//				loopexit->addAncestorNode(movone);
			//			}

			//checking
			//			assert(false); // please comment below to remove this assertion

			//			if(std::find(loopExitBlocks.begin(),loopExitBlocks.end(),node->getNode()->g)!=loopExitBlocks.end()){

			// if (BranchInst *BRI = dyn_cast<BranchInst>(node->getNode()))
			// {

			// 	for (int j = 0; j < BRI->getNumSuccessors(); ++j)
			// 	{
			// 		if (std::find(loopExitBlocks.begin(),
			// 					  loopExitBlocks.end(),
			// 					  BRI->getSuccessor(j)) != loopExitBlocks.end())
			// 		{

			// 			//current node belong to a loop exit basic block
			// 			dfgNode *loopexit = new dfgNode(this);
			// 			loopexit->setIdx(this->getNodesPtr()->size());
			// 			this->getNodesPtr()->push_back(loopexit);
			// 			loopexit->setNameType("LOOPEXIT");
			// 			loopexit->BB = BRI->getSuccessor(j);
			// 			loopexit->setLeftAlignedMemOp(1);

			// 			node->addChildNode(loopexit);
			// 			loopexit->addAncestorNode(node);

			// 			dfgNode *movone = new dfgNode(this);
			// 			movone->setIdx(this->getNodesPtr()->size());
			// 			this->getNodesPtr()->push_back(movone);
			// 			movone->setNameType("MOVC");
			// 			movone->BB = node->BB;
			// 			movone->setConstantVal(1);

			// 			movone->addChildNode(loopexit);
			// 			loopexit->addAncestorNode(movone);
			// 			exitFound = true;
			// 		}
			// 	}
			// }

			//			}
		}
	}

	return 0;
}

int DFG::handlestartstop_munit(std::vector<munitTransition> bbTrans)
{

	std::vector<dfgNode *> startNodes;
	std::map<dfgNode *, BasicBlock *> comingFromBBMap;
	std::map<dfgNode *, std::vector<BasicBlock *>> exitNodesWithSucc;
	int exitNodesWithSuccCount = 0;

	for (std::pair<Instruction *, dfgNode *> pair : LoopStartMap)
	{
		startNodes.push_back(pair.second);
		comingFromBBMap[pair.second] = pair.first->getParent();
	}

	dfgNode *realStartNode = NULL;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		dfgNode *node = NodeList[i];
		if (node->getNode() == NULL)
		{
			if (node->getNameType().compare("LOOPSTART") == 0)
			{
				assert(realStartNode == NULL);
				realStartNode = node;
			}
		}
		else
		{
			if (BranchInst *BRI = dyn_cast<BranchInst>(node->getNode()))
			{
				for (int j = 0; j < BRI->getNumSuccessors(); ++j)
				{
					if (std::find(loopBB.begin(),
							loopBB.end(),
							BRI->getSuccessor(j)) == loopBB.end())
					{
						//successor does not belong to the loop
						//						exitNodes.push_back(node);
						LLVM_DEBUG(BRI->dump());
						LLVM_DEBUG(dbgs() << "Successor BB : " << BRI->getSuccessor(j)->getName() << "\n");
						exitNodesWithSucc[node].push_back(BRI->getSuccessor(j));
						exitNodesWithSuccCount++;
					}
				}
			}
		}
	}
	assert(realStartNode != NULL);

	LLVM_DEBUG(dbgs() << "munitName = " << this->name << "\n");
	LLVM_DEBUG(dbgs() << "startNodes size =" << startNodes.size() << "\n");
	LLVM_DEBUG(dbgs() << "loopentryBB size =" << loopentryBB.size() << "\n");

	LLVM_DEBUG(dbgs() << "exitNodesWithSucc size =" << exitNodesWithSuccCount << "\n");
	LLVM_DEBUG(dbgs() << "loopexitBB size =" << loopexitBB.size() << "\n");

	assert(startNodes.size() == this->loopentryBB.size());
	assert(loopexitBB.size() == exitNodesWithSuccCount);

	std::vector<dfgNode *> connecttoRealStart;
	for (dfgNode *startNode : startNodes)
	{
		dfgNode *cusBitMsk = new dfgNode(this);
		cusBitMsk->setIdx(this->getNodesPtr()->size());
		this->getNodesPtr()->push_back(cusBitMsk);
		cusBitMsk->setNameType("PREDAND");
		cusBitMsk->BB = realStartNode->BB;

		bool found = false;
		uint32_t BBActiveTransBit;
		for (munitTransition munitTrans : bbTrans)
		{
			if (munitTrans.srcBB == comingFromBBMap[startNode] &&
					munitTrans.destBB == startNode->BB)
			{
				BBActiveTransBit = 1 << munitTrans.id;
				found = true;
				break;
			}
		}
		assert(found);
		cusBitMsk->setConstantVal(BBActiveTransBit);

		//make connections
		for (dfgNode *startNodeChild : startNode->getChildren())
		{
			startNode->removeChild(startNodeChild);
			startNodeChild->removeAncestor(startNode);
			removeEdge(findEdge(startNode, startNodeChild));

			cusBitMsk->addChildNode(startNodeChild);
			startNodeChild->addAncestorNode(cusBitMsk);
		}

		connecttoRealStart.push_back(cusBitMsk);
		//		assert(realStartNode->getNameType().compare("LOOPSTART")==0);
		//		realStartNode->addChildNode(cusBitMsk);
		//		cusBitMsk->addAncestorNode(realStartNode);
	}

	for (dfgNode *connectNode : connecttoRealStart)
	{
		assert(realStartNode->getNameType().compare("LOOPSTART") == 0);
		realStartNode->addChildNode(connectNode);
		connectNode->addAncestorNode(realStartNode);
	}

	//	uint32_t BBActiveWord = 0;
	//	for(BasicBlock* bb : loopentryBB){
	//		int bbIdx = (*bbids)[bb];
	//		int singlebit = 1 << bbIdx;
	//		BBActiveWord = BBActiveWord | singlebit;
	//	}
	//make connection for one mapping unit
	uint32_t BBActiveTransWord = 0;
	for (dfgNode *startNode : startNodes)
	{
		bool found = false;
		uint32_t BBActiveTransBit;

		for (munitTransition munitTrans : bbTrans)
		{
			if (munitTrans.srcBB == comingFromBBMap[startNode] &&
					munitTrans.destBB == startNode->BB)
			{
				BBActiveTransBit = 1 << munitTrans.id;
				BBActiveTransWord = BBActiveTransWord | BBActiveTransBit;
				found = true;
				break;
			}
		}
		assert(found);
	}

	dfgNode *cusZeroBitMsk = new dfgNode(this);
	cusZeroBitMsk->setIdx(this->getNodesPtr()->size());
	this->getNodesPtr()->push_back(cusZeroBitMsk);
	cusZeroBitMsk->setNameType("PREDAND");
	cusZeroBitMsk->BB = realStartNode->BB;
	cusZeroBitMsk->setConstantVal(~BBActiveTransWord);

	dfgNode *storeStart = new dfgNode(this);
	storeStart->setIdx(this->getNodesPtr()->size());
	this->getNodesPtr()->push_back(storeStart);
	storeStart->setNameType("STORESTART");
	storeStart->BB = realStartNode->BB;
	storeStart->setLeftAlignedMemOp(2);

	cusZeroBitMsk->addChildNode(storeStart);
	storeStart->addAncestorNode(cusZeroBitMsk);

	assert(realStartNode->getNameType().compare("LOOPSTART") == 0);
	realStartNode->addChildNode(cusZeroBitMsk);
	cusZeroBitMsk->addAncestorNode(realStartNode);

	for (std::pair<dfgNode *, std::vector<BasicBlock *>> pair : exitNodesWithSucc)
	{
		int count = 0;
		for (BasicBlock *succBasicBlock : pair.second)
		{
			dfgNode *node = pair.first;
			assert(pair.second.size() < 3);
			dfgNode *loopexit = new dfgNode(this);
			loopexit->setIdx(this->getNodesPtr()->size());
			this->getNodesPtr()->push_back(loopexit);
			loopexit->setNameType("LOOPEXIT");
			loopexit->BB = pair.second[0]; //just putting one bb out of the loop coz it doesnt really matter
			loopexit->setLeftAlignedMemOp(1);
			if (count == 1)
			{
				loopexit->setNPB(true);
			}
			node->addChildNode(loopexit);
			loopexit->addAncestorNode(node);

			dfgNode *movnextbb = new dfgNode(this);
			movnextbb->setIdx(this->getNodesPtr()->size());
			this->getNodesPtr()->push_back(movnextbb);
			movnextbb->setNameType("MOVC");
			movnextbb->BB = node->BB;

			bool found = false;
			uint32_t BBActiveTransBit = 0;
			for (munitTransition munitTrans : bbTrans)
			{
				if (munitTrans.srcBB == node->BB &&
						munitTrans.destBB == succBasicBlock)
				{
					BBActiveTransBit = 1 << munitTrans.id;
					found = true;
					break;
				}
			}
			movnextbb->setConstantVal(BBActiveTransBit);
			movnextbb->addChildNode(loopexit);
			loopexit->addAncestorNode(movnextbb);
			count++;
		}
	}
	return 0;
}

int DFG::nonGEPLoadStorecheck()
{
	dfgNode *node;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		if (node->getNode() == NULL)
			continue;
		if (node->getNode()->mayReadOrWriteMemory() != true)
			continue;

		const DataLayout DL = node->getNode()->getParent()->getParent()->getParent()->getDataLayout();

		//only comes to here when there are loads and nodes
		bool GEPfound = false;
		for (dfgNode *parent : node->getAncestors())
		{
			if (parent->getNode() == NULL)
				continue;
			if (dyn_cast<GetElementPtrInst>(parent->getNode()))
			{
				GEPfound = true;
				break;
			}
		}
		if (GEPfound)
			continue;

		Type *T;

		if (LoadInst *LDI = dyn_cast<LoadInst>(node->getNode()))
		{
			T = LDI->getPointerOperand()->getType();
			if (dyn_cast<Instruction>(LDI->getPointerOperand()))
			{
				continue;
			}
		}
		else if (StoreInst *STI = dyn_cast<StoreInst>(node->getNode()))
		{
			T = STI->getPointerOperand()->getType();
			if (dyn_cast<Instruction>(STI->getPointerOperand()))
			{
				continue;
			}
		}
		else
		{
			assert(false);
		}

		LLVM_DEBUG(node->getNode()->dump());
		int Size = DL.getTypeAllocSize(T);
		LLVM_DEBUG(dbgs() << "Size=" << Size << ",");

			if (Type *ValueType = morpher::llvm_compat::getLoadStoreValueType(node->getNode()))
			{
				LLVM_DEBUG(dbgs() << "ElementSize=" << DL.getTypeAllocSize(ValueType) << ",");
			}

		LLVM_DEBUG(dbgs() << "S/A/I/P=" << T->isStructTy() << "/");
		LLVM_DEBUG(dbgs() << T->isArrayTy() << "/");
		LLVM_DEBUG(dbgs() << T->isIntegerTy() << "/");
		LLVM_DEBUG(dbgs() << T->isPointerTy() << "\n");
	}

	assert(false);
}

int DFG::addMaskLowBitInstructions()
{
	for (dfgNode *node : NodeList)
	{
		if (node->getNode() != NULL)
		{
			if (node->getNode()->getOpcode() == Instruction::Shl)
			{
				int byteWidth = node->getNode()->getType()->getIntegerBitWidth() / 8;
				if (byteWidth == 2 || byteWidth == 1)
				{
					dfgNode *mls = new dfgNode(this);
					mls->setIdx(this->getNodesPtr()->size());
					this->getNodesPtr()->push_back(mls);
					mls->setNameType("MASKAND");
					if (byteWidth == 2)
					{
						mls->setConstantVal(0xffff);
					}
					else
					{ //byteWidth == 1
						mls->setConstantVal(0xff);
					}
					mls->BB = node->BB;

					dfgNode *child;
					for (int i = 0; i < node->getChildren().size(); ++i)
					{
						child = node->getChildren()[i];

						node->removeChild(child);
						child->removeAncestor(node);
						removeEdge(findEdge(node, child));

						node->addChildNode(mls);
						mls->addAncestorNode(node);

						mls->addChildNode(child);
						child->addAncestorNode(mls);
					}
				}
				else
				{
					assert(byteWidth == 4);
				}
			}
		}
	}
	return 0;
}

int DFG::addBreakLongerPaths()
{
	dfgNode *node;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		for (dfgNode *child : node->getChildren())
		{
			int ASAPdistance = child->getASAPnumber() - node->getASAPnumber();
			int internalASAPdist = ASAPdistance / 4;
			if (ASAPdistance > 5 && internalASAPdist > 0)
			{
				LLVM_DEBUG(dbgs() << "breaking long paths adding op:OR 0 between "
						<< ": node=" << node->getIdx() << "and node=" << child->getIdx() << "\n");

				dfgNode *orzero1 = new dfgNode(this);
				orzero1->setIdx(this->getNodesPtr()->size());
				this->getNodesPtr()->push_back(orzero1);
				orzero1->BB = node->BB;
				orzero1->setNameType("ORZERO");

				orzero1->setASAPnumber(node->getASAPnumber() + internalASAPdist);

				dfgNode *orzero2 = new dfgNode(this);
				orzero2->setIdx(this->getNodesPtr()->size());
				this->getNodesPtr()->push_back(orzero2);
				orzero2->BB = node->BB;
				orzero2->setNameType("ORZERO");

				orzero2->setASAPnumber(orzero1->getASAPnumber() + internalASAPdist);

				dfgNode *orzero3 = new dfgNode(this);
				orzero3->setIdx(this->getNodesPtr()->size());
				this->getNodesPtr()->push_back(orzero3);
				orzero3->BB = node->BB;
				orzero3->setNameType("ORZERO");

				orzero3->setASAPnumber(orzero2->getASAPnumber() + internalASAPdist);

				orzero3->setALAPnumber(child->getALAPnumber() - 1);
				orzero2->setALAPnumber(orzero3->getALAPnumber() - 1);
				orzero1->setALAPnumber(orzero2->getALAPnumber() - 1);

				node->removeChild(child);
				child->removeAncestor(node);

				node->addChildNode(orzero1);
				orzero1->addAncestorNode(node);

				orzero1->addChildNode(orzero2);
				orzero2->addAncestorNode(orzero1);

				orzero2->addChildNode(orzero3);
				orzero3->addAncestorNode(orzero2);

				orzero3->addChildNode(child);
				child->addAncestorNode(orzero3);
			}
		}
	}
	return 0;
}

int DFG::analyzeRTpaths()
{
	assert(!nodeRouteMap.empty());
	LLVM_DEBUG(dbgs() << "analyzeRTpaths :: begin\n");

	for (dfgNode *node : NodeList)
	{
		if (node->getAncestors().empty())
			continue;
		if (node->getNode() != NULL)
		{
			if (dyn_cast<PHINode>(node->getNode()))
			{
				continue;
			}
		}
		for (dfgNode *parent : node->getAncestors())
		{
			if (parent->getNameType().compare("OutLoopLOAD") == 0 ||
					parent->getNameType().compare("MOVC") == 0)
			{
				continue;
			}

			if (std::find(node->getPHIancestors().begin(),
					node->getPHIancestors().begin(),
					parent) != node->getPHIancestors().end())
			{
				continue;
			}

			//analyzing distance in time
			int distanceDt = 1; //it should be atleast 1
			int parent_tplus1_2 = parent_tplus1_2 = (parent->getMappedLoc()->getT() + 1) % currCGRA->getMII();
			if (parent->getMappedLoc()->getPEType() == MEM)
			{
				distanceDt = 2;
				parent_tplus1_2 = (parent->getMappedLoc()->getT() + 2) % currCGRA->getMII();
			}

			LLVM_DEBUG(dbgs() << "path size = " << nodeRouteMap[node][parent].size() << "\n");

			int parent_y = parent->getMappedLoc()->getY();
			int parent_x = parent->getMappedLoc()->getX();

			CGRANode *startingCnode = currCGRA->getCGRANode(parent_tplus1_2, parent_y, parent_x);

			for (int i = 1; i < nodeRouteMap[node][parent].size(); ++i)
			{

				//this vector is going from destination to source
				CGRANode *curr = nodeRouteMap[node][parent][i];
				CGRANode *next = nodeRouteMap[node][parent][i - 1];

				if (curr->getT() != next->getT())
				{
					LLVM_DEBUG(dbgs() << "curr=" << curr->getNameSp() << ",next=" << next->getNameSp() << "\n");
					distanceDt++;
				}

				if (curr == startingCnode)
				{
					break;
				}
			}

			int distanceRT = node->getmappedRealTime() - parent->getmappedRealTime();

			if (distanceDt != distanceRT)
			{
				LLVM_DEBUG(dbgs() << "the path from " << parent->getMappedLoc()->getNameSp() << ",Node=" << parent->getIdx());
				LLVM_DEBUG(dbgs() << " to " << node->getMappedLoc()->getNameSp() << ",Node=" << node->getIdx());
				LLVM_DEBUG(dbgs() << " RealDistance=" << distanceRT << " and MappedDistance=" << distanceDt << "\n");
			}

			assert(distanceRT % currCGRA->getMII() == distanceDt % currCGRA->getMII());
		}
	}
	LLVM_DEBUG(dbgs() << "analyzeRTpaths :: end\n");
	return 0;
}

std::map<BasicBlock *, std::set<BasicBlock *>> DFG::checkMutexBBs()
{
	assert(NodeList.size() > 0);
	dfgNode *firstNode = NodeList[0];
	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8> Result;
	FindFunctionBackedges(*(firstNode->BB->getParent()), Result);
	LLVM_DEBUG(dbgs() << "Number of Backedges = " << Result.size() << "\n");

	std::map<BasicBlock *, std::set<BasicBlock *>> mutexBBs;

	for (BasicBlock *BB1 : loopBB)
	{
		for (BasicBlock *BB2 : loopBB)
		{
			if (std::find(BBSuccBasicBlocks[BB1].begin(), BBSuccBasicBlocks[BB1].end(), BB2) == BBSuccBasicBlocks[BB1].end())
			{
				if (std::find(BBSuccBasicBlocks[BB2].begin(), BBSuccBasicBlocks[BB2].end(), BB1) == BBSuccBasicBlocks[BB2].end())
				{
					// coming here means BB2 is not a successor of BB1
					// coming here means BB1 is not a successor of BB2
					// then its mutually exclusive
					mutexBBs[BB1].insert(BB2);
					mutexBBs[BB2].insert(BB1);
				}
			}
		}
	}

	for (std::pair<BasicBlock *, std::set<BasicBlock *>> pair : mutexBBs)
	{
		LLVM_DEBUG(dbgs() << "BB=" << pair.first->getName() << ":");
		for (BasicBlock *BB2 : pair.second)
		{
			LLVM_DEBUG(dbgs() << BB2->getName() << ",");
		}
		LLVM_DEBUG(dbgs() << "\n");
	}

	return mutexBBs;
}

int DFG::printHyCUBEInsHist()
{
	LLVM_DEBUG(dbgs() << "Hist--------------------\n");
	for (std::pair<HyCUBEIns, int> pair : hyCUBEInsHist)
	{
		LLVM_DEBUG(dbgs() << "Ins:" << HyCUBEInsStrings[pair.first] << "," << pair.second << "\n");
	}

	return 0;
}

void DFG::printNewDFGXML()
{
	std::string fileName = name + "_DFG.xml";
	std::ofstream xmlFile;
	xmlFile.open(fileName.c_str());

	printREGIMapfiles();
	addPHIParents();
	classifyParents();
	MergeCMerge();

	//    insertMOVC();
	//	scheduleASAP();
	//	scheduleALAP();
	//	balanceASAPALAP();
	//				  LoopDFG.addBreakLongerPaths();
	CreateSchList();

	std::map<BasicBlock *, std::set<BasicBlock *>> mBBs = checkMutexBBs();
	std::map<std::string, std::set<std::string>> mBBs_str;

	std::map<int, std::vector<dfgNode *>> asaplevelNodeList;
	for (dfgNode *node : NodeList)
	{
		asaplevelNodeList[node->getASAPnumber()].push_back(node);
	}

	std::map<dfgNode *, std::string> nodeBBModified;
	for (dfgNode *node : NodeList)
	{
		nodeBBModified[node] = node->BB->getName().str();
	}

	for (dfgNode *node : NodeList)
	{
		int cmergeParentCount = 0;
		std::set<std::string> mutexBBs;
		for (dfgNode *parent : node->getAncestors())
		{
			if (HyCUBEInsStrings[parent->getFinalIns()] == "CMERGE")
			{
				nodeBBModified[parent] = nodeBBModified[parent] + "_" + std::to_string(node->getIdx()) + "_" + std::to_string(cmergeParentCount);
				mutexBBs.insert(nodeBBModified[parent]);
				cmergeParentCount++;
			}
		}
		for (std::string bb_str1 : mutexBBs)
		{
			for (std::string bb_str2 : mutexBBs)
			{
				if (bb_str2 == bb_str1)
					continue;
				mBBs_str[bb_str1].insert(bb_str2);
			}
		}
	}

	xmlFile << "<MutexBB>\n";
	for (std::pair<BasicBlock *, std::set<BasicBlock *>> pair : mBBs)
	{
		BasicBlock *first = pair.first;
		xmlFile << "<BB1 name=\"" << first->getName().str() << "\">\n";
		for (BasicBlock *second : pair.second)
		{
			xmlFile << "\t<BB2 name=\"" << second->getName().str() << "\"/>\n";
		}
		xmlFile << "</BB1>\n";
	}
	for (std::pair<std::string, std::set<std::string>> pair : mBBs_str)
	{
		std::string first = pair.first;
		xmlFile << "<BB1 name=\"" << first << "\">\n";
		for (std::string second : pair.second)
		{
			xmlFile << "\t<BB2 name=\"" << second << "\"/>\n";
		}
		xmlFile << "</BB1>\n";
	}
	xmlFile << "</MutexBB>\n";

	xmlFile << "<DFG count=\"" << NodeList.size() << "\">\n";

	for (dfgNode *node : NodeList)
	{
		//	for (int i = 0; i < maxASAPLevel; ++i) {
		//		for(dfgNode* node : asaplevelNodeList[i]){
		xmlFile << "<Node idx=\"" << node->getIdx() << "\"";
		xmlFile << " ASAP=\"" << node->getASAPnumber() << "\"";

		//		    if(node->getNameType() == "OutLoopLOAD") {
		//		    	xmlFile << "OutLoopLOAD=\"1\"";
		//		    }
		//		    else{
		//		    	xmlFile << "OutLoopLOAD=\"0\"";
		//		    }
		//
		//		    if(node->getNameType() == "OutLoopSTORE") {
		//		    	xmlFile << "OutLoopSTORE=\"1\"";
		//		    }
		//		    else{
		//		    	xmlFile << "OutLoopSTORE=\"0\"";
		//		    }

		//		    xmlFile << "BB=\"" << node->BB->getName().str() << "\"";
		xmlFile << " BB=\"" << nodeBBModified[node] << "\"";
		if (node->hasConstantVal())
		{
			xmlFile << " CONST=\"" << node->getConstantVal() << "\"";
		}
		xmlFile << ">\n";

		xmlFile << "<OP>";
		if ((node->getNameType() == "OutLoopLOAD") || (node->getNameType() == "OutLoopSTORE"))
		{
			xmlFile << "O";
		}
		xmlFile << HyCUBEInsStrings[node->getFinalIns()] << "</OP>\n";
		//			xmlFile << "<OP>" << HyCUBEInsStrings[node->getFinalIns()] << "</OP>\n";

		xmlFile << "<Inputs>\n";
		for (dfgNode *parent : node->getAncestors())
		{
			//			xmlFile << "\t<Input idx=\"" << parent->getIdx() << "\" type=\"DATA\"/>\n";
			xmlFile << "\t<Input idx=\"" << parent->getIdx() << "\"/>\n";
		}
		//		for(dfgNode* parentPHI : node->getPHIancestors()){
		//			xmlFile << "\t<Input idx=\"" << parentPHI->getIdx() << "\" type=\"PHI\"/>\n";
		//		}
		xmlFile << "</Inputs>\n";

		xmlFile << "<Outputs>\n";
		for (dfgNode *child : node->getChildren())
		{
			//			xmlFile << "\t<Output idx=\"" << child->getIdx() <<"\" type=\"DATA\"/>\n";
			xmlFile << "\t<Output idx=\"" << child->getIdx() << "\" ";

			if (child->parentClassification[0] == node)
			{
				xmlFile << "type=\"P\"/>\n";
			}
			else if (child->parentClassification[1] == node)
			{
				xmlFile << "type=\"I1\"/>\n";
			}
			else if (child->parentClassification[2] == node)
			{
				xmlFile << "type=\"I2\"/>\n";
			}
			else
			{
				bool found = false;
				for (std::pair<int, dfgNode *> pair : child->parentClassification)
				{
					if (pair.second == node)
					{
						xmlFile << "type=\"I2\"/>\n";
						found = true;
						break;
					}
				}

				assert(found);
			}
		}
		//		for(dfgNode* phiChild : node->getPHIchildren()){
		//			xmlFile << "\t<Output idx=\"" << phiChild->getIdx() << "\" type=\"PHI\"/>\n";
		//		}
		xmlFile << "</Outputs>\n";

		xmlFile << "<RecParents>\n";
		for (dfgNode *recParent : node->getRecAncestors())
		{
			xmlFile << "\t<RecParent idx=\"" << recParent->getIdx() << "\"";
			xmlFile << " type=\"" << node->RecAncestorType[recParent->getNode()] << "\"";
			xmlFile << "/>\n";
		}
		xmlFile << "</RecParents>\n";

		xmlFile << "</Node>\n\n";
	}
	//		}
	//	}

	xmlFile << "</DFG>\n";
	xmlFile.close();
}

void DFG::printREGIMapfiles()
{
	std::map<HyCUBEIns, int> regimapInsIdxMap;
	regimapInsIdxMap[NOP] = 32;
	regimapInsIdxMap[ADD] = 0;
	regimapInsIdxMap[SUB] = 1;
	regimapInsIdxMap[MUL] = 2;
	regimapInsIdxMap[SEXT] = 4;
	regimapInsIdxMap[DIV] = 3;
	regimapInsIdxMap[LS] = 4;
	regimapInsIdxMap[RS] = 5;
	regimapInsIdxMap[ARS] = regimapInsIdxMap[RS];
	regimapInsIdxMap[AND] = 6;
	regimapInsIdxMap[OR] = 7;
	regimapInsIdxMap[XOR] = 8;
	regimapInsIdxMap[SELECT] = 29;
	regimapInsIdxMap[CMERGE] = regimapInsIdxMap[OR];
	regimapInsIdxMap[CMP] = 10;
	regimapInsIdxMap[CLT] = 12;
	regimapInsIdxMap[BR] = 31;
	regimapInsIdxMap[CGT] = 9;

	regimapInsIdxMap[Hy_LOAD] = 19;
	regimapInsIdxMap[Hy_LOADH] = 19;
	regimapInsIdxMap[Hy_LOADB] = 19;
	regimapInsIdxMap[Hy_STORE] = 21;
	regimapInsIdxMap[Hy_STOREH] = 21;
	regimapInsIdxMap[Hy_STOREB] = 21;
	regimapInsIdxMap[JUMPL] = regimapInsIdxMap[BR];
	regimapInsIdxMap[LOADCL] = regimapInsIdxMap[Hy_LOAD];
	regimapInsIdxMap[MOVCL] = regimapInsIdxMap[OR];
	regimapInsIdxMap[MOVC] = regimapInsIdxMap[OR];

	//NOP,ADD,SUB,MUL,SEXT,DIV,LS,RS,ARS,AND,OR,XOR,SELECT,CMERGE,CMP,CLT,BR,CGT,LOADCL,MOVCL,Hy_LOAD,Hy_LOADH,Hy_LOADB,Hy_STORE,Hy_STOREH,Hy_STOREB,JUMPL,MOVC

	std::string nodefileName = name + "_REGI_node.txt";
	std::string edgefileName = name + "_REGI_edge.txt";

	std::ofstream nodeFile;
	nodeFile.open(nodefileName.c_str());

	std::ofstream edgeFile;
	edgeFile.open(edgefileName.c_str());

	int additionalDataNodeStartIdx = 1000;

	for (dfgNode *node : NodeList)
	{
		if (regimapInsIdxMap[node->getFinalIns()] == 19 || regimapInsIdxMap[node->getFinalIns()] == 21)
		{
			nodeFile << node->getIdx() << "\t" << regimapInsIdxMap[node->getFinalIns()] << "\n";
			nodeFile << additionalDataNodeStartIdx << "\t" << regimapInsIdxMap[node->getFinalIns()] + 1 << "\n";

			if (regimapInsIdxMap[node->getFinalIns()] == 19)
			{
				edgeFile << node->getIdx() << "\t" << additionalDataNodeStartIdx << "\tLRE\n";
			}
			else
			{
				assert(regimapInsIdxMap[node->getFinalIns()] == 21);
				edgeFile << node->getIdx() << "\t" << additionalDataNodeStartIdx << "\tSRE\n";
			}

			for (dfgNode *child : node->getChildren())
			{
				edgeFile << additionalDataNodeStartIdx << "\t" << child->getIdx() << "\tTRU\n";
			}
			for (dfgNode *child : node->getPHIchildren())
			{
				edgeFile << additionalDataNodeStartIdx << "\t" << child->getIdx() << "\tPRE\n";
			}
			additionalDataNodeStartIdx++;
		}
		else
		{
			nodeFile << node->getIdx() << "\t" << regimapInsIdxMap[node->getFinalIns()] << "\n";
			for (dfgNode *child : node->getChildren())
			{
				edgeFile << node->getIdx() << "\t" << child->getIdx() << "\tTRU\n";
			}
			for (dfgNode *child : node->getPHIchildren())
			{
				edgeFile << node->getIdx() << "\t" << child->getIdx() << "\tPRE\n";
			}
		}
	}

	nodeFile.close();
	edgeFile.close();
}

void DFG::MergeCMerge()
{

	std::set<dfgNode *> delNodes;

	for (dfgNode *node : NodeList)
	{
		if (node->getNameType().compare("CMERGE") == 0)
		{
			assert(node->parentClassification.find(0) != node->parentClassification.end());
			//			assert(node->parentClassification.find(1)!=node->parentClassification.end());
			if (node->parentClassification.find(1) == node->parentClassification.end())
			{
				continue; // no data parent
			}

			dfgNode *predParent = node->parentClassification[0];
			dfgNode *dataParent = node->parentClassification[1];

			if (dataParent->getAncestors().size() > 0)
			{
				if (dataParent->parentClassification.find(0) != dataParent->parentClassification.end())
				{
					continue;
				}
			}

			if (dataParent->getChildren().size() == 1)
			{
				predParent->removeChild(node);
				node->removeAncestor(predParent);
				removeEdge(findEdge(predParent, node));

				dataParent->removeChild(node);
				node->removeAncestor(dataParent);
				removeEdge(findEdge(dataParent, node));

				delNodes.insert(node);

				predParent->addChildNode(dataParent);
				dataParent->addAncestorNode(predParent);
				dataParent->parentClassification[0] = predParent;

				std::vector<dfgNode *> children = node->getChildren();
				for (dfgNode *child : children)
				{
					dataParent->addChildNode(child);
					child->addAncestorNode(dataParent);

					LLVM_DEBUG(dbgs() << "parentClassification size = " << child->parentClassification.size() << "\n");
					//					if(child->parentClassification[0]==node){
					//						child->parentClassification[0]=dataParent;
					//					}
					//					else if(child->parentClassification[1]==node){
					//						child->parentClassification[1]=dataParent;
					//					}
					//					else if(child->parentClassification[2]==node){
					//						child->parentClassification[2]=dataParent;
					//					}
					//					else{
					//						assert(false);
					//					}
					bool found = false;
					for (std::pair<int, dfgNode *> pair : child->parentClassification)
					{
						if (pair.second == node)
						{
							child->parentClassification[pair.first] = dataParent;
							found = true;
							break;
						}
					}
					assert(found);

					node->removeChild(child);
					child->removeAncestor(node);
					removeEdge(findEdge(node, child));
				}
			}
		}
	}

	for (dfgNode *del : delNodes)
	{
		NodeList.erase(std::find(NodeList.begin(), NodeList.end(), del));
	}
}

void DFG::insertMOVC()
{
	int beforeNodeListSize = NodeList.size();
	std::vector<dfgNode *> tobeadded;

	for (dfgNode *node : NodeList)
	{
		if (node->hasConstantVal())
		{
			dfgNode *temp = new dfgNode(this);
			temp->setNameType("MOVC");
			temp->setIdx(NodeList.size() + tobeadded.size());
			temp->BB = node->BB;
			temp->setFinalIns(MOVC);
			temp->setConstantVal(node->getConstantVal());
			tobeadded.push_back(temp);

			temp->addChildNode(node);
			node->addAncestorNode(temp);
			node->parentClassification[2] = temp;
		}
	}

	for (dfgNode *newNode : tobeadded)
	{
		NodeList.push_back(newNode);
	}

	LLVM_DEBUG(dbgs() << "NodeList Size was increased : from=" << beforeNodeListSize << ", to=" << NodeList.size() << "\n");
}

void DFG::removeDisconnectedNodes()
{

	std::set<dfgNode *> rn;

	for (dfgNode *n : NodeList)
	{
		bool noAncestors = true;
		bool ps_edges_found = false;

		std::vector<dfgNode *> ancestors = n->getAncestors();
		for (dfgNode *anc : ancestors)
		{
			if (findEdge(anc, n)->getType() == EDGE_TYPE_PS)
			{
				ps_edges_found = true;
			}
			else
			{
				noAncestors = false;
			}
		}

		if (ps_edges_found && noAncestors)
		{
			for (dfgNode *anc : ancestors)
			{
				anc->removeChild(n);
				n->removeAncestor(anc);
			}
		}

		if (noAncestors && n->getChildren().size() == 0)
		{
			rn.insert(n);
		}
	}

	for (dfgNode *n : rn)
	{
		LLVM_DEBUG(dbgs() << "Removing Node = " << n->getIdx() << "\n");
		NodeList.erase(std::remove(NodeList.begin(), NodeList.end(), n), NodeList.end());
	}
}

void DFG::removeAGINodes()
{
	// Step 1: Seed AGI set with GEP and synthetic helper nodes
	std::unordered_set<dfgNode *> agiSet;
	for (dfgNode *node : NodeList)
	{
		if (node->isGEP() ||
			node->getNameType() == "GEPLEFTSHIFT" ||
			node->getNameType() == "GEPSCALE" ||
			node->getNameType() == "GEPADD")
		{
			agiSet.insert(node);
		}
	}
	// Step 2: Propagate upward — if ALL data children of a node are AGI, it's AGI too
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (dfgNode *node : NodeList)
		{
			if (agiSet.count(node))
				continue;
			const auto children = node->getChildren();
			if (children.empty())
				continue;
			bool allAGI = true;
			for (dfgNode *child : children)
			{
				if (!agiSet.count(child))
				{
					allAGI = false;
					break;
				}
			}
			if (allAGI)
			{
				agiSet.insert(node);
				changed = true;
			}
		}
	}
	// Step 3: Emit AGI sub-graph XML alongside the main DFG before pruning.
	// Boundary references to non-AGI partners use the post-removal new index
	// so downstream tooling can cross-reference with the main DFG XML.
	{
		std::map<dfgNode *, int> agiLocalIdx;
		std::map<dfgNode *, int> postRemoveIdx;
		int local = 0;
		int post = 0;
		for (dfgNode *n : NodeList)
		{
			if (agiSet.count(n))
				agiLocalIdx[n] = local++;
			else
				postRemoveIdx[n] = post++;
		}

		std::string fileName = name + "_AGI_subgraph.xml";
		std::ofstream xmlFile;
		xmlFile.open(fileName.c_str());
		xmlFile << "<AGI_DFG count=\"" << agiLocalIdx.size() << "\">\n";
		for (dfgNode *n : NodeList)
		{
			if (!agiSet.count(n))
				continue;
			xmlFile << "<Node localIdx=\"" << agiLocalIdx[n] << "\"";
			if (n->BB)
				xmlFile << " BB=\"" << n->BB->getName().str() << "\"";
			if (n->hasConstantVal())
				xmlFile << " CONST=\"" << n->getConstantVal() << "\"";
			xmlFile << ">\n";
			std::string op = n->getNameType();
			if (op.empty() && n->getFinalIns() != NOP)
				op = HyCUBEInsStrings[n->getFinalIns()];
			xmlFile << "\t<OP>" << op << "</OP>\n";
			xmlFile << "\t<Inputs>\n";
			for (dfgNode *p : n->getAncestors())
			{
				if (agiSet.count(p))
					xmlFile << "\t\t<Input localIdx=\"" << agiLocalIdx[p] << "\"/>\n";
				else
					xmlFile << "\t\t<Input externalIdx=\"" << postRemoveIdx[p] << "\"/>\n";
			}
			xmlFile << "\t</Inputs>\n";
			xmlFile << "\t<Outputs>\n";
			for (dfgNode *c : n->getChildren())
			{
				if (agiSet.count(c))
					xmlFile << "\t\t<Output localIdx=\"" << agiLocalIdx[c] << "\"/>\n";
				else
					xmlFile << "\t\t<Output externalIdx=\"" << postRemoveIdx[c] << "\"/>\n";
			}
			xmlFile << "\t</Outputs>\n";
			xmlFile << "</Node>\n";
		}
		xmlFile << "</AGI_DFG>\n";
		xmlFile.close();
		llvm::outs() << "[removeAGINodes] Wrote AGI sub-graph (" << agiLocalIdx.size()
					 << " nodes) to " << fileName << "\n";

		std::string manifestName = name + "_AGI_manifest.json";
		std::ofstream manifestFile;
		manifestFile.open(manifestName.c_str());
		if (!manifestFile.is_open())
		{
			std::string error = "Could not open AGI manifest for writing: " + manifestName;
			llvm::report_fatal_error(llvm::StringRef(error));
		}

		auto nodeOp = [this](dfgNode *node) {
			std::string op = node->getNameType();
			if (op.empty() && node->getFinalIns() != NOP)
				op = HyCUBEInsStrings[node->getFinalIns()];
			return op;
		};

		int boundaryInputs = 0;
		int boundaryOutputs = 0;
		for (dfgNode *n : NodeList)
		{
			if (!agiSet.count(n))
				continue;
			for (dfgNode *p : n->getAncestors())
				if (!agiSet.count(p))
					boundaryInputs++;
			for (dfgNode *c : n->getChildren())
				if (!agiSet.count(c))
					boundaryOutputs++;
		}

		manifestFile << "{\n";
		manifestFile << "  \"schema_version\": 1,\n";
		manifestFile << "  \"transform\": \"externalize_agi\",\n";
		manifestFile << "  \"dfg_name\": " << jsonString(name) << ",\n";
		manifestFile << "  \"original_node_count\": " << NodeList.size() << ",\n";
		manifestFile << "  \"removed_node_count\": " << agiLocalIdx.size() << ",\n";
		manifestFile << "  \"remaining_node_count\": " << postRemoveIdx.size() << ",\n";
		manifestFile << "  \"boundary_input_count\": " << boundaryInputs << ",\n";
		manifestFile << "  \"boundary_output_count\": " << boundaryOutputs << ",\n";
		manifestFile << "  \"nodes\": [\n";

		bool firstNode = true;
		for (dfgNode *n : NodeList)
		{
			if (!agiSet.count(n))
				continue;
			if (!firstNode)
				manifestFile << ",\n";
			firstNode = false;

			manifestFile << "    {\n";
			manifestFile << "      \"local_idx\": " << agiLocalIdx[n] << ",\n";
			manifestFile << "      \"original_idx\": " << n->getIdx() << ",\n";
			manifestFile << "      \"op\": " << jsonString(nodeOp(n)) << ",\n";
			manifestFile << "      \"name_type\": " << jsonString(n->getNameType()) << ",\n";
			manifestFile << "      \"final_instruction\": " << jsonString(HyCUBEInsStrings[n->getFinalIns()]) << ",\n";
			manifestFile << "      \"bb\": " << jsonString(n->BB ? n->BB->getName().str() : "") << ",\n";
			manifestFile << "      \"base_pointer\": " << jsonString(n->getArrBasePtr()) << ",\n";
			manifestFile << "      \"is_gep\": " << (n->isGEP() ? "true" : "false") << ",\n";
			manifestFile << "      \"constant\": ";
			if (n->hasConstantVal())
				manifestFile << n->getConstantVal();
			else
				manifestFile << "null";
			manifestFile << ",\n";

			auto writeRefs = [&](const char *fieldName, const std::vector<dfgNode *> &refs) {
				manifestFile << "      \"" << fieldName << "\": [";
				bool firstRef = true;
				for (dfgNode *ref : refs)
				{
					if (!firstRef)
						manifestFile << ", ";
					firstRef = false;
					manifestFile << "{";
					manifestFile << "\"original_idx\": " << ref->getIdx() << ", ";
					if (agiSet.count(ref))
					{
						manifestFile << "\"kind\": \"agi\", ";
						manifestFile << "\"local_idx\": " << agiLocalIdx[ref];
					}
					else
					{
						manifestFile << "\"kind\": \"external\", ";
						manifestFile << "\"external_idx\": " << postRemoveIdx[ref];
					}
					manifestFile << "}";
				}
				manifestFile << "]";
			};

			writeRefs("inputs", n->getAncestors());
			manifestFile << ",\n";
			writeRefs("outputs", n->getChildren());
			manifestFile << "\n";
			manifestFile << "    }";
		}
		manifestFile << "\n";
		manifestFile << "  ]\n";
		manifestFile << "}\n";
		manifestFile.close();
		llvm::outs() << "[removeAGINodes] Wrote AGI manifest to " << manifestName << "\n";
	}

	// Step 4: Disconnect AGI nodes from remaining non-AGI nodes
	for (dfgNode *agi : agiSet)
	{
		for (dfgNode *parent : agi->getAncestors())
		{
			if (!agiSet.count(parent))
				parent->removeChild(agi);
		}
		for (dfgNode *child : agi->getChildren())
		{
			if (!agiSet.count(child))
				child->removeAncestor(agi);
		}
	}
	// Step 5: Remove from NodeList
	NodeList.erase(std::remove_if(NodeList.begin(), NodeList.end(),
								  [&agiSet](dfgNode *n)
								  { return agiSet.count(n) > 0; }),
				   NodeList.end());
	// Step 6: Re-index
	for (size_t i = 0; i < NodeList.size(); i++)
		NodeList[i]->setIdx((int)i);
	llvm::outs() << "[removeAGINodes] Remaining nodes after AGI removal: " << NodeList.size() << "\n";
}

void DFG::removeIteratorOnlyControl(std::map<dfgNode*, dfgNode*>* cmergeCtrlInputs)
{
	// The trace supplies memory addresses but not predicate events, so only
	// control that is data-disconnected from remaining mapped work can be
	// removed. One predicate crossing is additionally supported: a reduction
	// carry CMERGE consumes the loop-latch CMP purely as "loop continues".
	// Its output feeds the phi SELECT of the *next* iteration, so within the
	// executed iterations the carry must fire on every iteration (the final
	// unconsumed token is harmless once the iterator is externalized) — the
	// predicate edge is simply dropped, and the counter/CMP component becomes
	// removable even for register-carried (unrolled) reductions. The kept
	// LOOPSTART event still sequences the init CMERGE.
	auto isBoundaryNode = [](dfgNode* node) {
		const std::string type = node->getNameType();
		return type == "LOOPSTART" || type == "STORESTART" || type == "LOOPEXIT";
	};
	auto isControlCandidate = [](dfgNode* node) {
		const std::string type = node->getNameType();
		if (type == "LOOPSTART" || type == "STORESTART" || type == "LOOPEXIT" ||
			type == "OutLoopLOAD" || type == "OutLoopSTORE" || type == "CMERGE")
			return true;
		switch (node->getFinalIns())
		{
		case ADD:
		case SUB:
		case CMP:
		case SELECT:
		case CMERGE:
		case MOVC:
			return true;
		default:
			return false;
		}
	};
	auto describeNode = [this](dfgNode* node) {
		std::string op = node->getNameType();
		if (op.empty() && node->getFinalIns() != NOP)
			op = HyCUBEInsStrings[node->getFinalIns()];
		return "idx=" + std::to_string(node->getIdx()) + " op=" + op;
	};
	auto fail = [](const std::string& message) {
		report_fatal_error(llvm::StringRef(message));
	};
	auto writeSkippedManifest = [this](const std::string& reason) {
		std::ofstream manifestFile(name + "_ITER_manifest.json");
		if (!manifestFile.is_open())
			report_fatal_error("Could not write skipped iterator manifest");
		manifestFile << "{\n";
		manifestFile << "  \"schema_version\": 1,\n";
		manifestFile << "  \"transform\": \"externalize_iterator_only\",\n";
		manifestFile << "  \"status\": \"skipped\",\n";
		manifestFile << "  \"reason\": " << jsonString(reason) << ",\n";
		manifestFile << "  \"original_node_count\": " << NodeList.size() << ",\n";
		manifestFile << "  \"removed_node_count\": 0,\n";
		manifestFile << "  \"final_node_count\": " << NodeList.size() << ",\n";
		manifestFile << "  \"removed_nodes\": [],\n";
		manifestFile << "  \"artifact_vars_pruned\": [],\n";
		manifestFile << "  \"index_map\": [\n";
		for (size_t index = 0; index < NodeList.size(); ++index)
		{
			if (index != 0) manifestFile << ",\n";
			manifestFile << "    {\"original_idx\": " << index
			             << ", \"final_idx\": " << index << "}";
		}
		manifestFile << "\n  ]\n";
		manifestFile << "}\n";
		llvm::outs() << "[agi_iter] Skipped iterator externalization: "
		             << reason << "\n";
	};

	bool hasBoundary = false;
	for (dfgNode* node : NodeList)
		if (isBoundaryNode(node))
			hasBoundary = true;
	if (!hasBoundary)
		report_fatal_error(
			"agi_iter requires canonical loop boundary nodes after AGI externalization");

	std::unordered_set<dfgNode*> inGraph(NodeList.begin(), NodeList.end());

	auto isPredEdge = [&](dfgNode* parent, dfgNode* child) {
		if (parent->getNameType() == "LOOPSTART")
			return true;
		if (cmergeCtrlInputs && child->getNameType() == "CMERGE")
		{
			auto found = cmergeCtrlInputs->find(child);
			if (found != cmergeCtrlInputs->end() && found->second == parent)
				return true;
		}
		auto slot0 = child->parentClassification.find(0);
		return slot0 != child->parentClassification.end() && slot0->second == parent;
	};

	// Partition the graph into components connected by data edges; predicate
	// edges do not merge components, so control that only *predicates* work
	// stays separable while control that *feeds* work merges into it.
	std::unordered_map<dfgNode*, dfgNode*> ufParent;
	for (dfgNode* node : NodeList)
		ufParent[node] = node;
	auto findRoot = [&ufParent](dfgNode* node) {
		while (ufParent[node] != node)
		{
			ufParent[node] = ufParent[ufParent[node]];
			node = ufParent[node];
		}
		return node;
	};
	auto unite = [&](dfgNode* a, dfgNode* b) { ufParent[findRoot(a)] = findRoot(b); };
	for (dfgNode* node : NodeList)
		for (dfgNode* parent : node->getAncestors())
			if (inGraph.count(parent) && !isPredEdge(parent, node))
				unite(parent, node);

	struct ComponentInfo
	{
		bool allCandidates = true;
		bool special = false; // holds the first-iteration event (LOOPSTART/STORESTART)
	};
	std::unordered_map<dfgNode*, ComponentInfo> comps;
	for (dfgNode* node : NodeList)
	{
		ComponentInfo& info = comps[findRoot(node)];
		if (!isControlCandidate(node))
			info.allCandidates = false;
		const std::string type = node->getNameType();
		if (type == "LOOPSTART" || type == "STORESTART")
			info.special = true;
	}
	auto isRemovalCandidate = [&](dfgNode* node) {
		const ComponentInfo& info = comps[findRoot(node)];
		return info.allCandidates && !info.special;
	};

	dfgNode* loopstart = NULL;
	bool multipleLoopstarts = false;
	for (dfgNode* node : NodeList)
		if (node->getNameType() == "LOOPSTART")
		{
			if (loopstart)
				multipleLoopstarts = true;
			loopstart = node;
		}

	// Removal-candidate components exchanging predicates (the latch CMP
	// predicates the other counters' CMERGEs and the LOOPEXIT store) must be
	// decided together: cluster them. Uniting only same-flag components keeps
	// the comps flags valid.
	for (dfgNode* node : NodeList)
		if (isRemovalCandidate(node))
			for (dfgNode* parent : node->getAncestors())
				if (inGraph.count(parent) && isRemovalCandidate(parent))
					unite(parent, node);

	struct Rewire
	{
		dfgNode* cmerge;
		dfgNode* oldCtrl;
	};
	std::unordered_map<dfgNode*, std::vector<Rewire>> clusterRewires;
	std::unordered_map<dfgNode*, std::string> clusterBlockReason;

	for (dfgNode* node : NodeList)
	{
		if (!isRemovalCandidate(node))
			continue;
		dfgNode* cluster = findRoot(node);
		if (clusterBlockReason.count(cluster))
			continue;

		for (dfgNode* parent : node->getAncestors())
		{
			if (!inGraph.count(parent) || isRemovalCandidate(parent))
				continue;
			if (parent->getNameType() == "LOOPSTART")
				continue; // trigger edges from the kept event die with the removal
			clusterBlockReason[cluster] = "iterator control " + describeNode(node) +
				" consumes " + describeNode(parent);
			break;
		}
		if (clusterBlockReason.count(cluster))
			continue;

		for (dfgNode* child : node->getChildren())
		{
			if (!inGraph.count(child) || isRemovalCandidate(child))
				continue;
			// Every crossing edge is a predicate by construction: a data edge
			// would have merged the two components.
			std::string blocked;
			if (!isPredEdge(node, child))
				blocked = "has unexpected data edge to " + describeNode(child);
			else if (comps[findRoot(child)].special)
				blocked = "predicates kept boundary node " + describeNode(child);
			else if (child->getNameType() != "CMERGE" || !cmergeCtrlInputs ||
			         !cmergeCtrlInputs->count(child) ||
			         (*cmergeCtrlInputs)[child] != node)
				blocked = "predicates non-rewireable work " + describeNode(child);
			else if (node->getFinalIns() != CMP)
				blocked = "predicates work CMERGE but is not the latch CMP";
			else if (!loopstart || multipleLoopstarts)
				blocked = "predicates work CMERGE without a unique LOOPSTART event";
			else
			{
				// The CMERGE must fire on loop-continue: the opposite polarity
				// of every LOOPEXIT this CMP drives.
				bool sawExit = false;
				bool polarityOk = true;
				for (dfgNode* exitChild : node->getChildren())
					if (exitChild->getNameType() == "LOOPEXIT")
					{
						sawExit = true;
						if (exitChild->getNPB() == child->getNPB())
							polarityOk = false;
					}
				if (!sawExit)
					blocked = "predicates work CMERGE but drives no LOOPEXIT";
				else if (!polarityOk)
					blocked = "predicates work CMERGE " + describeNode(child) +
						" that does not fire on loop-continue";
			}
			if (!blocked.empty())
			{
				clusterBlockReason[cluster] =
					"iterator control " + describeNode(node) + " " + blocked;
				break;
			}
			clusterRewires[cluster].push_back({child, node});
		}
	}

	std::unordered_set<dfgNode*> iteratorSet;
	std::vector<Rewire> rewires;
	std::string firstBlockReason;
	for (dfgNode* node : NodeList)
	{
		if (!isRemovalCandidate(node))
			continue;
		dfgNode* cluster = findRoot(node);
		if (clusterBlockReason.count(cluster))
		{
			if (firstBlockReason.empty())
				firstBlockReason = clusterBlockReason[cluster];
			continue;
		}
		iteratorSet.insert(node);
	}
	for (auto& pair : clusterRewires)
		if (!clusterBlockReason.count(pair.first))
			rewires.insert(rewires.end(), pair.second.begin(), pair.second.end());

	if (iteratorSet.empty())
	{
		writeSkippedManifest(firstBlockReason.empty()
			? "no separable iterator-only control component"
			: firstBlockReason);
		return;
	}

	// When nothing surviving consumes the LOOPSTART event, fold the event
	// machinery (LOOPSTART/STORESTART and their constants) into the removal —
	// the memory-carried-reduction case (unroll factor 1).
	std::unordered_set<dfgNode*> specials;
	for (dfgNode* node : NodeList)
	{
		const ComponentInfo& info = comps[findRoot(node)];
		if (info.special && info.allCandidates)
			specials.insert(node);
	}
	bool loopstartConsumed = !rewires.empty();
	if (!loopstartConsumed)
		for (dfgNode* node : specials)
			for (dfgNode* child : node->getChildren())
				if (inGraph.count(child) && !iteratorSet.count(child) && !specials.count(child))
					loopstartConsumed = true;
	if (!loopstartConsumed)
	{
		bool foldable = true;
		for (dfgNode* node : specials)
			for (dfgNode* parent : node->getAncestors())
				if (inGraph.count(parent) && !iteratorSet.count(parent) && !specials.count(parent))
					foldable = false;
		if (foldable)
			iteratorSet.insert(specials.begin(), specials.end());
	}

	std::vector<std::pair<int, int>> rewireRecords; // {cmerge idx, old ctrl idx} pre-reindex
	for (const Rewire& rw : rewires)
	{
		rewireRecords.push_back({rw.cmerge->getIdx(), rw.oldCtrl->getIdx()});
		rw.cmerge->removeAncestor(rw.oldCtrl);
		rw.oldCtrl->removeChild(rw.cmerge);
		cmergeCtrlInputs->erase(rw.cmerge);
		llvm::outs() << "[agi_iter] Unpredicated reduction carry " << describeNode(rw.cmerge)
		             << " (was gated by " << describeNode(rw.oldCtrl) << ")\n";
	}

	// Drop trigger links from kept nodes (the LOOPSTART event) into the
	// removed component; the closure checks below verify nothing else crosses.
	for (dfgNode* node : iteratorSet)
		for (dfgNode* parent : node->getAncestors())
			if (inGraph.count(parent) && !iteratorSet.count(parent))
			{
				parent->removeChild(node);
				node->removeAncestor(parent);
			}

	for (dfgNode* node : iteratorSet)
	{
		for (dfgNode* parent : node->getAncestors())
			if (!iteratorSet.count(parent))
				fail("agi_iter control component has a surviving parent: " +
					describeNode(parent));
		for (dfgNode* child : node->getChildren())
			if (!iteratorSet.count(child))
				fail("agi_iter control component has a surviving child: " +
					describeNode(child));
	}

	std::unordered_map<dfgNode*, int> originalIndex;
	for (dfgNode* node : NodeList)
		originalIndex[node] = node->getIdx();
	const int originalNodeCount = (int)NodeList.size();

	edgeList.erase(
		std::remove_if(edgeList.begin(), edgeList.end(),
			[&iteratorSet](Edge& edge) {
				return iteratorSet.count(edge.getSrc()) || iteratorSet.count(edge.getDest());
			}),
		edgeList.end());
	NodeList.erase(
		std::remove_if(NodeList.begin(), NodeList.end(),
			[&iteratorSet](dfgNode* node) { return iteratorSet.count(node) > 0; }),
		NodeList.end());
	for (auto it = OutLoopNodeMap.begin(); it != OutLoopNodeMap.end(); )
		if (iteratorSet.count(it->second)) it = OutLoopNodeMap.erase(it); else ++it;
	for (auto it = OutLoopNodeMapReverse.begin(); it != OutLoopNodeMapReverse.end(); )
		if (iteratorSet.count(it->first)) it = OutLoopNodeMapReverse.erase(it); else ++it;
	for (auto it = LoopStartMap.begin(); it != LoopStartMap.end(); )
		if (iteratorSet.count(it->second)) it = LoopStartMap.erase(it); else ++it;

	for (size_t index = 0; index < NodeList.size(); ++index)
	{
		NodeList[index]->setIdx((int)index);
		NodeList[index]->parentClassification.clear();
		NodeList[index]->childClassification.clear();
	}
	for (dfgNode* node : NodeList)
	{
		for (dfgNode* parent : node->getAncestors())
			if (iteratorSet.count(parent))
				report_fatal_error("agi_iter left a surviving node with a removed parent");
		for (dfgNode* child : node->getChildren())
			if (iteratorSet.count(child))
				report_fatal_error("agi_iter left a surviving node with a removed child");
	}

	std::ofstream manifestFile(name + "_ITER_manifest.json");
	if (!manifestFile.is_open())
		report_fatal_error("Could not open iterator manifest for writing");
	manifestFile << "{\n";
	manifestFile << "  \"schema_version\": 1,\n";
	manifestFile << "  \"transform\": \"externalize_iterator_only\",\n";
	manifestFile << "  \"status\": \"applied\",\n";
	manifestFile << "  \"original_node_count\": " << originalNodeCount << ",\n";
	manifestFile << "  \"removed_node_count\": " << iteratorSet.size() << ",\n";
	manifestFile << "  \"final_node_count\": " << NodeList.size() << ",\n";
	manifestFile << "  \"removed_nodes\": [";
	bool firstRemoved = true;
	std::vector<int> removedIndices;
	removedIndices.reserve(iteratorSet.size());
	for (dfgNode* node : iteratorSet)
		removedIndices.push_back(originalIndex[node]);
	std::sort(removedIndices.begin(), removedIndices.end());
	for (int index : removedIndices)
	{
		if (!firstRemoved) manifestFile << ", ";
		firstRemoved = false;
		manifestFile << index;
	}
	manifestFile << "],\n";
	bool keptLoopstartEvent = false;
	bool keptLoopexit = false;
	for (dfgNode* node : NodeList)
	{
		const std::string type = node->getNameType();
		if (type == "LOOPSTART" || type == "STORESTART")
			keptLoopstartEvent = true;
		if (type == "LOOPEXIT")
			keptLoopexit = true;
	}
	loopstartEventKept = keptLoopstartEvent;
	loopexitKept = keptLoopexit;
	// UpdateSPMAllocation wrote mem_alloc before this transform ran and omits
	// the boundary entries for agi_iter; surviving boundary nodes still need
	// their addresses, so append them here.
	if (keptLoopstartEvent || keptLoopexit)
	{
		std::ofstream memAllocAppend(kernelname + "_mem_alloc.txt", std::ios::app);
		if (!memAllocAppend.is_open())
			report_fatal_error("Could not append boundary entries to mem_alloc");
		if (keptLoopexit)
			memAllocAppend << "loopend" << "," << (MEM_SIZE / 2 - 1) << "\n";
		if (keptLoopstartEvent)
			memAllocAppend << "loopstart" << "," << (MEM_SIZE - 2) << "\n";
	}
	manifestFile << "  \"artifact_vars_pruned\": [";
	{
		bool firstVar = true;
		if (!keptLoopstartEvent)
		{
			manifestFile << "\"loopstart\"";
			firstVar = false;
		}
		if (!keptLoopexit)
		{
			if (!firstVar) manifestFile << ", ";
			manifestFile << "\"loopend\"";
		}
	}
	manifestFile << "],\n";
	manifestFile << "  \"unpredicated_carries\": [";
	{
		bool firstRewire = true;
		for (const std::pair<int, int>& record : rewireRecords)
		{
			if (!firstRewire) manifestFile << ", ";
			firstRewire = false;
			manifestFile << "{\"cmerge_original_idx\": " << record.first
			             << ", \"old_ctrl_original_idx\": " << record.second << "}";
		}
	}
	manifestFile << "],\n";
	manifestFile << "  \"index_map\": [";
	bool firstMap = true;
	for (dfgNode* node : NodeList)
	{
		if (!firstMap) manifestFile << ", ";
		firstMap = false;
		manifestFile << "{\"original_idx\": " << originalIndex[node]
				 << ", \"final_idx\": " << node->getIdx() << "}";
	}
	manifestFile << "]\n";
	manifestFile << "}\n";
	iteratorExternalized = true;
	llvm::outs() << "[removeIteratorOnlyControl] Removed " << iteratorSet.size()
				 << " iterator/control nodes; remaining nodes=" << NodeList.size() << "\n";
}
