#include "DfgPartPredInternal.h"

void DFGPartPred::connectBB() {

	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>,8> BackedgeBBs;
	dfgNode firstNode = *NodeList[0];
	FindFunctionBackedges(*(firstNode.getNode()->getFunction()),BackedgeBBs);

	for(dfgNode* node : NodeList){
		if(node->getNode()==NULL) continue;
		if(BranchInst* BRI = dyn_cast<BranchInst>(node->getNode())){
			for(int i=0; i<BRI->getNumSuccessors(); i++){

				const BasicBlock* currBB = BRI->getParent();
				const BasicBlock* succBB = BRI->getSuccessor(i);
				std::pair<const BasicBlock *, const BasicBlock *> BBCouple = std::make_pair(currBB,succBB);

				bool isBackEdge = false;
				if(std::find(BackedgeBBs.begin(),BackedgeBBs.end(),BBCouple) != BackedgeBBs.end()){
					isBackEdge = true;
				}


				bool isConditional = BRI->isConditional();
				bool conditionVal = true;
				if(isConditional){
					if(i == 1){
						conditionVal = false;
					}
				}

				std::vector<dfgNode*> stores = getStoreInstructions(BRI->getSuccessor(i)); //this doesnt include phinodes

				if(node->getAncestors().size()!=1){
					LLVM_DEBUG(BRI->dump());
					LLVM_DEBUG(dbgs() << "BR node ancestor size = " << node->getAncestors().size() << "\n");
				}
				//				assert(node->getAncestors().size()==1); //should be a unique parent;
				//				dfgNode* BRParent = node->getAncestors()[0];

				for(dfgNode* BRParent : node->getAncestors()){

					if(!isConditional){
						const BasicBlock* BRParentBB = BRParent->BB;
						const BranchInst* BRParentBRI = cast<BranchInst>(BRParentBB->getTerminator());
						//						assert(BRParentBRI->isConditional());
						//						if(BRParentBRI->getSuccessor(0)==currBB){
						//							conditionVal=true;
						//						}
						//						else if(BRParentBRI->getSuccessor(1)==currBB){
						//							conditionVal=false;
						//						}
						//						else{
						//							BRParentBRI->dump();
						//							LLVM_DEBUG(dbgs() << BRParentBRI->getSuccessor(0)->getName() << "\n";
						//							LLVM_DEBUG(dbgs() << BRParentBRI->getSuccessor(1)->getName() << "\n";
						//							LLVM_DEBUG(dbgs() << currBB->getName() << "\n";
						//							LLVM_DEBUG(dbgs() << BRParent->getIdx() << "\n";
						//							assert(false);
						//						}
						std::set<const BasicBlock*> searchSoFar;
						DFG_CHECK(checkBRPath(BRParentBRI,currBB,conditionVal,searchSoFar),
							"connectBB",
							"failed to resolve branch path from parent BB to current BB (unsupported control flow pattern)");
						isConditional=true; //this is always true. coz there is no backtoback unconditional branches.
					}

					LLVM_DEBUG(dbgs() << "BR : ");
					LLVM_DEBUG(BRI->dump());
					LLVM_DEBUG(dbgs() << " and BRParent :" << BRParent->getIdx() << "\n");


					LLVM_DEBUG(dbgs() << "leafs from basicblock=" << currBB->getName() << " for basicblock=" << succBB->getName().str() << "\n");
					for(dfgNode* succNode : stores){
						LLVM_DEBUG(dbgs() << succNode->getIdx() << ",");
					}
					LLVM_DEBUG(dbgs() << "\n");

					for(dfgNode* succNode : stores){
						isBackEdge = checkBackEdge(node,succNode);
						BRParent->addChildNode(succNode,EDGE_TYPE_DATA,isBackEdge,isConditional,conditionVal);
						succNode->addAncestorNode(BRParent,EDGE_TYPE_DATA,isBackEdge,isConditional,conditionVal);
					}

				}
			}
		}
	}


}

void DFGPartPred::removeOutLoopLoad() {
	LLVM_DEBUG(dbgs() << "Removing outloop loads begin...\n");
	std::set<dfgNode*> removalNodes;
	for(dfgNode* node : NodeList){

		if(node->getNameType() == "OutLoopLOAD"){
			bool oloadonlyparent=false;
			for(dfgNode* child : node->getChildren()){
				if(child->getAncestors().size() == 1){
					oloadonlyparent=true;
					break;
				}
			}
			if(oloadonlyparent) continue;

			removalNodes.insert(node);
			DFG_CHECK(node->getAncestors().empty(),
				"removeOutLoopLoad",
				"OutLoopLOAD node " + std::to_string(node->getIdx()) + " unexpectedly has ancestors");

			for(dfgNode* child : node->getChildren()){
				node->removeChild(child);
				child->removeAncestor(node);
			}
		}

	}

	for(dfgNode* rn : removalNodes){
		LLVM_DEBUG(dbgs() << "Removing Node = " << rn->getIdx() << "\n");
		NodeList.erase(std::remove(NodeList.begin(), NodeList.end(), rn), NodeList.end());
	}

	name = name + "_noSemiOLOAD";

	scheduleASAP();
	scheduleALAP();

	LLVM_DEBUG(dbgs() << "Removing outloop loads end...\n");
}


std::vector<dfgNode*> DFGPartPred::getStoreInstructions(BasicBlock* BB) {

	std::vector<dfgNode*> res;
	for(dfgNode* node : NodeList){
		if(node->BB != BB) continue;
		if(node->getNode()){
			if(StoreInst* STI = dyn_cast<StoreInst>(node->getNode())){
				bool parentInThisBB = false;
				//				for(dfgNode* parent : node->getAncestors()){
				//					if(parent->BB == BB) parentInThisBB = true; break;
				//				}
				if(!parentInThisBB) res.push_back(node);
			}
			else if(BranchInst* BRI = dyn_cast<BranchInst>(node->getNode())){
				if(!BRI->isConditional()){
					res.push_back(node);
				}
			}
			//			else if(PHINode* PHI = dyn_cast<PHINode>(node->getNode())){
			//				res.push_back(node);
			//			}
		}
		else if(node->getNameType() == "OutLoopSTORE"){
			bool parentInThisBB = false;
			//			for(dfgNode* parent : node->getAncestors()){
			//				if(parent->BB == BB) parentInThisBB = true; break;
			//			}
			if(!parentInThisBB) res.push_back(node);
		}
	}
	return res;
}
/*
 * This function
 * 1) connects PHI nodes with its parents by analyzing PHI instruction[value,basic block][value, basic block]
 * 2) add CMERGE nodes between PHI nodes and their parents (+control conditions)
 * 3) add LOOPSTART control node
 *
 * PHI is implemented as SELECT instruction in HyCUBE
 *
 * SELECT nodes select a valid data input and forward it to output
 * CMERGE nodes generate valid data based on a branch outcome
 *
 * Therefore SELECT nodes should be connected through CMERGE nodes
 *
 * */
int DFGPartPred::handlePHINodes(std::set<BasicBlock*> LoopBB) {

	std::vector<dfgNode*> phiNodes;
	for(dfgNode* node : NodeList){
		if(node->getNode()==NULL)continue;
		if(PHINode* PHI = dyn_cast<PHINode>(node->getNode())){
			phiNodes.push_back(node);
		}
	}


	for(dfgNode* node : phiNodes){
		if(node->getNode()==NULL)continue;
		if(PHINode* PHI = dyn_cast<PHINode>(node->getNode())){
			std::vector<dfgNode*> mergeNodes;
			std::map<dfgNode*,std::pair<dfgNode*,bool>> mergeNodeControl;

			DFG_CHECK(node->getAncestors().empty(),
				"handlePHINodes",
				"PHI node " + std::to_string(node->getIdx()) + " should have no ancestors before PHI handling, but has " +
				std::to_string(node->getAncestors().size()));

			LLVM_DEBUG(dbgs() << "DEBUG........... begin\n");
			LLVM_DEBUG(dbgs() << "PHI :: ");
			LLVM_DEBUG(PHI->dump());


			for (int i = 0; i < PHI->getNumIncomingValues(); ++i) {
				BasicBlock* bb = PHI->getIncomingBlock(i);
				Value* V = PHI->getIncomingValue(i);
				LLVM_DEBUG(dbgs() << "V :: ");LLVM_DEBUG( V->dump());
				//				bool incomingCTRLVal;
				std::map<dfgNode*,bool> cnodectrlvalmap;

				//				dfgNode* previousCTRLNode;
				std::vector<dfgNode*> previousCtrlNodes;

				if(LoopBB.find(bb) == LoopBB.end()){
					//Not found
					std::pair<BasicBlock*,BasicBlock*> bbPair = std::make_pair(bb,(BasicBlock*)node->BB);
					assert(loopentryBB.find(bbPair)!=loopentryBB.end());
					//					previousCTRLNode = getStartNode(bb,node);
					previousCtrlNodes.push_back(getStartNode(bb,node));
					//					incomingCTRLVal=true; // all start nodes are assumed to be true
					cnodectrlvalmap[getStartNode(bb,node)]=true;
				}
				else{ // within the loop
					BranchInst* BRI = cast<BranchInst>(bb->getTerminator());
					dfgNode* BRNode = findNode(BRI);
					bool isConditional = BRI->isConditional();

					//					BRI->dump();
					//					assert(BRNode->getAncestors().size()==1);
					//					previousCTRLNode = BRNode->getAncestors()[0];


					if(!isConditional){
						const BasicBlock* currBB = BRI->getParent();
						previousCtrlNodes = BRNode->getAncestors();

						for(dfgNode* pcn : previousCtrlNodes){
							const BasicBlock* BRParentBB = pcn->BB;
							const BranchInst* BRParentBRI = cast<BranchInst>(BRParentBB->getTerminator());
							assert(BRParentBRI->isConditional());
							// if(BRParentBRI->getSuccessor(0)==currBB){
							// 	incomingCTRLVal=true;
							// }
							// else if(BRParentBRI->getSuccessor(1)==currBB){
							// 	incomingCTRLVal=false;
							// }
							// else{
							// 	BRParentBRI->dump();
							// 	LLVM_DEBUG(dbgs() << BRParentBRI->getSuccessor(0)->getName() << "\n";
							// 	LLVM_DEBUG(dbgs() << BRParentBRI->getSuccessor(1)->getName() << "\n";
							// 	LLVM_DEBUG(dbgs() << currBB->getName() << "\n";
							// 	LLVM_DEBUG(dbgs() << previousCTRLNode->getIdx() << "\n";
							// 	assert(false);
							// }
							std::set<const BasicBlock*> searchSoFar;
							bool incomingCTRLVal;
							DFG_CHECK(checkBRPath(BRParentBRI,currBB,incomingCTRLVal,searchSoFar),
								"handlePHINodes",
								"failed to resolve branch path for PHI node " + std::to_string(node->getIdx()) +
								" (unsupported control flow pattern in PHI incoming blocks)");
							cnodectrlvalmap[pcn]=incomingCTRLVal;
							isConditional=true; //this is always true. coz there is no backtoback unconditional branches.

						}

						}
						else{
							DFG_CHECK(BRNode->getAncestors().size()==1,
								"handlePHINodes",
								"expected exactly one controlling branch ancestor for PHI node " +
								std::to_string(node->getIdx()) +
								", found " + std::to_string(BRNode->getAncestors().size()) +
								" (unsupported multi-control PHI pattern)");
							//						previousCTRLNode = BRNode->getAncestors()[0];
							previousCtrlNodes.push_back(BRNode->getAncestors()[0]);
						if(BRI->getSuccessor(0) == node->BB){
							//							incomingCTRLVal = true;
							cnodectrlvalmap[BRNode->getAncestors()[0]]=true;
						}
						else{
							//							incomingCTRLVal = false;
							cnodectrlvalmap[BRNode->getAncestors()[0]]=false;
							assert(BRI->getSuccessor(1) == node->BB);
						}
					}

					for(dfgNode* pcn : previousCtrlNodes) {
						LLVM_DEBUG(dbgs() << "pcn idx=" << pcn->getIdx() << ",");
						if(pcn->getNode()){
							LLVM_DEBUG(dbgs() << "previousCTRLNode : "); LLVM_DEBUG(pcn->getNode()->dump());
						}
						else{
							LLVM_DEBUG(dbgs() << "\n");
						}
						LLVM_DEBUG(dbgs() << "CTRL VAL : " << cnodectrlvalmap[pcn] << "\n");
					}

				}
				//				assert(previousCTRLNode!=NULL);
				assert(!previousCtrlNodes.empty());

				for(dfgNode* previousCTRLNode : previousCtrlNodes){
					bool incomingCTRLVal = cnodectrlvalmap[previousCTRLNode];
					dfgNode* mergeNode;
					if(ConstantInt* CI = dyn_cast<ConstantInt>(V)){
						int constant = CI->getSExtValue();
						mergeNode = insertMergeNode(node,previousCTRLNode,incomingCTRLVal,constant);

					}
					else if(ConstantFP* FP = dyn_cast<ConstantFP>(V)){
						int constant = (int)FP->getValueAPF().convertToFloat();
						mergeNode = insertMergeNode(node,previousCTRLNode,incomingCTRLVal,constant);

					}
					else if(UndefValue *UND = dyn_cast<UndefValue>(V)){
						int constant = 0;
						mergeNode = insertMergeNode(node,previousCTRLNode,incomingCTRLVal,constant);
					}
					//					else if(sizeArrMap.find(V->getName().str())!=sizeArrMap.end()){
					//						int constant = sizeArrMap[V->getName().str()];
					//						mergeNode = insertMergeNode(node,previousCTRLNode,incomingCTRLVal,constant);
					//					}
					else if(Argument *ARG = dyn_cast<Argument>(V)){
						//FIXME : need cleaner way link the argument value when implementing the backend
						int constant = 0;
						mergeNode = insertMergeNode(node,previousCTRLNode,incomingCTRLVal,constant);
						PHIArgMap[node][V]=mergeNode;
					}
					else{

						dfgNode* phiParent = NULL;
						if(Instruction* ins = dyn_cast<Instruction>(V)){
							phiParent = findNode(ins);
						}
						bool isPhiParentOutLoopLoad=false;
						if(phiParent == NULL){ //not found
							isPhiParentOutLoopLoad=true;
							if(OutLoopNodeMap.find(V)!=OutLoopNodeMap.end()){
								phiParent=OutLoopNodeMap[V];
							}
							else{
								phiParent=addLoadParent(V,node);
								bool isBackEdge = checkBackEdge(previousCTRLNode,phiParent);
								//							previousCTRLNode->addChildNode(phiParent,EDGE_TYPE_DATA,isBackEdge,true,incomingCTRLVal);
								//							phiParent->addAncestorNode(previousCTRLNode,EDGE_TYPE_DATA,isBackEdge,true,incomingCTRLVal);
							}
						}
						assert(phiParent!=NULL);


						bool isPhiParentSuccCTRL=false;
						//					for (int succ = 0; succ < previousCTRLNode->BB->getTerminator()->getNumSuccessors(); ++succ) {
						//						if(previousCTRLNode->BB->getTerminator()->getSuccessor(succ)==phiParent->BB){
						//							isPhiParentSuccCTRL=true;
						//							break;
						//						}
						//					}

						//					if(previousCTRLNode->BB != phiParent->BB){
						if(!isPhiParentSuccCTRL){
							mergeNode = insertMergeNode(node,previousCTRLNode,incomingCTRLVal,phiParent);
						}
						else{
							LLVM_DEBUG(dbgs() << "mergeNode and control are from the same BB\n");
							mergeNode = phiParent;
							//						if(!isPhiParentOutLoopLoad){
							//							mergeNodeControl[mergeNode]=std::make_pair(previousCTRLNode,incomingCTRLVal);
							//						}
						}
					}
					mergeNodes.push_back(mergeNode);
				}
			}
			LLVM_DEBUG(dbgs() << "DEBUG........... end\n");
			bool recursivePhiNodes=false;

			for(dfgNode* child : node->getChildren()){
				for(dfgNode* mergeNode : mergeNodes){
					if(child == mergeNode){
						LLVM_DEBUG(dbgs() << "recursivePhiNodes found!!!!\n");
						recursivePhiNodes=true;
						break;
					}

					if(checkPHILoop(mergeNode,node)){
						recursivePhiNodes=true;
						break;
					}
				}
				if(recursivePhiNodes) break;
			}

			LLVM_DEBUG(dbgs() << "USERS ::::::::::::\n");
			for(User* u : PHI->users()){
				LLVM_DEBUG(u->dump());
				if(PHINode* phiChildIns = dyn_cast<PHINode>(u)){
					if(dfgNode* phichildnode = findNode(phiChildIns)){
						recursivePhiNodes=true;
						break;
					}
				}
			}

			assert(node->getAncestors().empty());

			//			assert(!backedgeChildMergeNodes.empty());

			//This is hack for keep phinodes being removed
			// UPDATE : i just keep the induction variable
			bool isInductionVar=false;
			// if(PHI == currLoop->getCanonicalInductionVariable()){
			// 	recursivePhiNodes=true;
			// 	isInductionVar=true;
			// }

			//cmerge node has backedges, its better to keep the phi node
			for(dfgNode* mergeNode : mergeNodes){
				if(backedgeChildMergeNodes.find(mergeNode)!=backedgeChildMergeNodes.end()){
					recursivePhiNodes=true;
				}
			}

			if(recursivePhiNodes){
				for(dfgNode* mergeNode : mergeNodes){

					bool isBackEdge;
					if(backedgeChildMergeNodes.find(mergeNode)!=backedgeChildMergeNodes.end()){
						isBackEdge = true;
					}
					else{
						isBackEdge = checkBackEdge(mergeNode,node);
					}

					//this is if we decide to keep the phi nodes then there children should be backedges
					//if one of the ancestor is a backedge
					//					if(isBackEdge){
					//						for(dfgNode* child : node->getChildren()){
					//							node->childBackEdgeMap[child]=true;
					//							child->ancestorBackEdgeMap[node]=true;
					//						}
					//					}
					//					isBackEdge = false;

					//					bool isBackEdge = checkBackEdge(actPHI,node);

					if(!(isInductionVar && isBackEdge)){
						if(isInductionVar) {
							LLVM_DEBUG(dbgs() << "this is induction variable, node=" << node->getIdx() << ",mergeNode=" << mergeNode->getIdx() << "\n");
							assert(isBackEdge == false);
						}
						node->addAncestorNode(mergeNode,EDGE_TYPE_DATA,isBackEdge);
						mergeNode->addChildNode(node,EDGE_TYPE_DATA,isBackEdge);
					}
				}
			}
			else{
				std::vector<dfgNode*> phiChildren = node->getChildren();
				for(dfgNode* child : phiChildren){

					node->removeChild(child);
					child->removeAncestor(node);

					LLVM_DEBUG(dbgs() << "PHIChild : " << child->getIdx() << " : ");

					for(dfgNode* mergeNode : mergeNodes){
						LLVM_DEBUG(dbgs() << mergeNode->getIdx() << ",");

						bool isBackEdge;
						if(backedgeChildMergeNodes.find(mergeNode)!=backedgeChildMergeNodes.end()){
							isBackEdge = true;
						}
						else{
							isBackEdge = checkBackEdge(mergeNode,child);
						}

						//						bool isBackEdge = checkBackEdge(actPHI,child);


						child->addAncestorNode(mergeNode,EDGE_TYPE_DATA,isBackEdge);
						mergeNode->addChildNode(child,EDGE_TYPE_DATA,isBackEdge);

						if(mergeNodeControl.find(mergeNode) != mergeNodeControl.end()){
							dfgNode* control = mergeNodeControl[mergeNode].first;
							bool controlVal = mergeNodeControl[mergeNode].second;

							child->addAncestorNode(control,EDGE_TYPE_DATA,isBackEdge,true,controlVal);
							control->addChildNode(child,EDGE_TYPE_DATA,isBackEdge,true,controlVal);
						}
					}
					LLVM_DEBUG(dbgs() << "\n");
				}
			}



			//			if(!node->getChildren().empty()){
			//				PHI->dump();
			//				assert(false);
			//			}
		}
	}


	return 0;
}

dfgNode* DFGPartPred::getStartNode(BasicBlock* BB, dfgNode* PHINode) {
	if(startNodes.find(BB)!=startNodes.end()){
		return startNodes[BB];
	}
	else{
		dfgNode* tempBR = new dfgNode(this);
		tempBR->setNameType("LOOPSTART");
		//		tempBR->BB = PHINode->BB;
		tempBR->BB = BB;
		tempBR->setIdx(NodeList.size());
		NodeList.push_back(tempBR);

		int nextStartNodesIdx = startNodes.size();
		startNodes[BB]=tempBR;
		startNodeConsts[BB]=nextStartNodesIdx;

		tempBR->setConstantVal(nextStartNodesIdx);
		return tempBR;
	}
}

dfgNode* DFGPartPred::insertMergeNode(dfgNode* PHINode, dfgNode* ctrl, bool controlVal, dfgNode* data) {
	dfgNode* temp = new dfgNode(this);

	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>,8> BackedgeBBs;
	FindFunctionBackedges(*(PHINode->getNode()->getFunction()),BackedgeBBs);
	const BasicBlock* ctrlBB = ctrl->BB;
	const BasicBlock* dataBB = data->BB;
	const BasicBlock* phiBB  = PHINode->BB;

	bool isCTRL2PHIBackEdge=false;
	bool isDATA2PHIBackEdge=false;

	//	std::pair<const BasicBlock *, const BasicBlock *> bbCuple;
	//	bbCuple = std::make_pair(ctrlBB,phiBB);
	//	if(std::find(BackedgeBBs.begin(),BackedgeBBs.end(),bbCuple)!=BackedgeBBs.end()){
	//		isCTRL2PHIBackEdge=true;
	//	}
	//	isCTRL2PHIBackEdge = checkBackEdge(ctrl,PHINode);
	isCTRL2PHIBackEdge=false;

	cmergePHINodes[temp]=PHINode;

	//	bbCuple = std::make_pair(dataBB,phiBB);
	//	if(std::find(BackedgeBBs.begin(),BackedgeBBs.end(),bbCuple)!=BackedgeBBs.end()){
	//		isDATA2PHIBackEdge=true;
	//	}
	//	isDATA2PHIBackEdge = checkBackEdge(data,PHINode);
	isDATA2PHIBackEdge = isCTRL2PHIBackEdge;

	temp->setNameType("CMERGE");
	//	temp->BB = PHINode->BB;

	if(startNodes.find((BasicBlock*)ctrl->BB) != startNodes.end()){
		temp->BB = PHINode->BB;
	}
	else{
		temp->BB = ctrl->BB;
		if(checkBackEdge(ctrl,PHINode) || ctrl->BB == PHINode->BB) backedgeChildMergeNodes.insert(temp);
	}

	temp->setIdx(NodeList.size());
	NodeList.push_back(temp);

	cmergeCtrlInputs[temp]=ctrl;
	cmergeDataInputs[temp]=data;

	temp->addAncestorNode(ctrl,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);
	ctrl->addChildNode(temp,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);

	temp->addAncestorNode(data,EDGE_TYPE_DATA,isDATA2PHIBackEdge,false);
	data->addChildNode(temp,EDGE_TYPE_DATA,isDATA2PHIBackEdge,false);
	return temp;
}

dfgNode* DFGPartPred::insertMergeNodeBeforeSEL(dfgNode* SELNode, dfgNode* ctrl, bool controlVal, dfgNode* data) {//for select Node
	dfgNode* temp = new dfgNode(this);

	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>,8> BackedgeBBs;
	//FindFunctionBackedges(*(PHINode->getNode()->getFunction()),BackedgeBBs);
	const BasicBlock* ctrlBB = ctrl->BB;
	const BasicBlock* dataBB = data->BB;
	const BasicBlock* selBB  = SELNode->BB;

	bool isCTRL2PHIBackEdge=false;
	bool isDATA2PHIBackEdge=false;

	isCTRL2PHIBackEdge=false;

	cmergePHINodes[temp]=SELNode;//create cmergeSELNodes?

	isDATA2PHIBackEdge = isCTRL2PHIBackEdge;

	temp->setNameType("CMERGE");

//	if(startNodes.find((BasicBlock*)ctrl->BB) != startNodes.end()){
//		temp->BB = PHINode->BB;
//	}
//	else{
//		temp->BB = ctrl->BB;
//		if(checkBackEdge(ctrl,PHINode) || ctrl->BB == PHINode->BB) backedgeChildMergeNodes.insert(temp);
//	}
	temp->BB = SELNode->BB;

	temp->setIdx(NodeList.size());
	NodeList.push_back(temp);

	cmergeCtrlInputs[temp]=ctrl;
	cmergeDataInputs[temp]=data;

	temp->addAncestorNode(ctrl,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);
	ctrl->addChildNode(temp,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);

	temp->addAncestorNode(data,EDGE_TYPE_DATA,isDATA2PHIBackEdge,false);
	data->addChildNode(temp,EDGE_TYPE_DATA,isDATA2PHIBackEdge,false);
	return temp;
}

dfgNode* DFGPartPred::insertMergeNodeBeforeSEL(dfgNode* SELNode, dfgNode* ctrl,
		bool controlVal, int val) {

	dfgNode* temp = new dfgNode(this);

	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>,8> BackedgeBBs;
	FindFunctionBackedges(*(SELNode->getNode()->getFunction()),BackedgeBBs);
	const BasicBlock* ctrlBB = ctrl->BB;
	const BasicBlock* selBB  = SELNode->BB;

	bool isCTRL2PHIBackEdge=false;

	cmergePHINodes[temp]=SELNode;


	temp->setNameType("CMERGE");

	cmergeCtrlInputs[temp]=ctrl;


//	if(startNodes.find((BasicBlock*)ctrl->BB) != startNodes.end()){
//		temp->BB = SELNode->BB;
//	}
//	else{
//		temp->BB = ctrl->BB;
//		if(checkBackEdge(ctrl,PHINode) || ctrl->BB == PHINode->BB) backedgeChildMergeNodes.insert(temp);
//	}
	temp->BB = SELNode->BB;


	temp->setIdx(NodeList.size());
	NodeList.push_back(temp);

	temp->addAncestorNode(ctrl,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);
	ctrl->addChildNode(temp,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);
	temp->setConstantVal(val);
	return temp;
}


dfgNode* DFGPartPred::insertMergeNode(dfgNode* PHINode, dfgNode* ctrl,
		bool controlVal, int val) {

	dfgNode* temp = new dfgNode(this);

	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>,8> BackedgeBBs;
	FindFunctionBackedges(*(PHINode->getNode()->getFunction()),BackedgeBBs);
	const BasicBlock* ctrlBB = ctrl->BB;
	const BasicBlock* phiBB  = PHINode->BB;

	bool isCTRL2PHIBackEdge=false;

	cmergePHINodes[temp]=PHINode;

	//	std::pair<const BasicBlock *, const BasicBlock *> bbCuple;
	//	bbCuple = std::make_pair(ctrlBB,phiBB);
	//	if(std::find(BackedgeBBs.begin(),BackedgeBBs.end(),bbCuple)!=BackedgeBBs.end()){
	//		isCTRL2PHIBackEdge=true;
	//	}
	//	isCTRL2PHIBackEdge = checkBackEdge(ctrl,PHINode);
	//	backedgeChildMergeNodes.insert(temp);

	temp->setNameType("CMERGE");
	//	temp->BB = PHINode->BB;

	cmergeCtrlInputs[temp]=ctrl;


	if(startNodes.find((BasicBlock*)ctrl->BB) != startNodes.end()){
		temp->BB = PHINode->BB;
	}
	else{
		temp->BB = ctrl->BB;
		if(checkBackEdge(ctrl,PHINode) || ctrl->BB == PHINode->BB) backedgeChildMergeNodes.insert(temp);
	}


	temp->setIdx(NodeList.size());
	NodeList.push_back(temp);

	temp->addAncestorNode(ctrl,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);
	ctrl->addChildNode(temp,EDGE_TYPE_DATA,isCTRL2PHIBackEdge,true,controlVal);
	temp->setConstantVal(val);
	return temp;
}

void DFGPartPred::removeDisconnetedNodes() {
	std::vector<dfgNode*> disconnectedNodes;
	for(dfgNode* node : NodeList){
		if(node->getChildren().size() == 0 && node->getAncestors().size() == 0){
			disconnectedNodes.push_back(node);
		}

		if(node->getNode()!=NULL){
			if(BranchInst* BRI = dyn_cast<BranchInst>(node->getNode())){
				assert(node->getChildren().empty());
				for(dfgNode* parent : node->getAncestors()){
					parent->removeChild(node);
					node->removeAncestor(parent);
				}
				disconnectedNodes.push_back(node);
			}
		}

	}

	for(dfgNode* removeNode : disconnectedNodes){
		NodeList.erase(std::remove(NodeList.begin(),NodeList.end(),removeNode), NodeList.end());
	}
	LLVM_DEBUG(dbgs() << "POST REMOVAL NODE COUNT = " << NodeList.size() << "\n");
}
/*
 * This function add CMERGE nodes between SELECT node and its non-CMERGE parents.
 * SELECT nodes select a valid data input and forward it to output
 * CMERGE nodes generate valid data based on a branch outcome
 *
 * Therefore SELECT nodes should be connected through CMERGE nodes
 *
 *
 * */
int DFGPartPred::handleSELECTNodes() {

	std::vector<dfgNode*> selectNodes;
	for(dfgNode* node : NodeList){
		if(node->getNode()==NULL) continue;
		if(SelectInst* SLI = dyn_cast<SelectInst>(node->getNode())){
			bool isLeafIns=true;
			LLVM_DEBUG(SLI->dump());
			for(dfgNode* parent : node->getAncestors()){
				if(parent->BB == node->BB){
					isLeafIns=false;
					break;
				}
			}

			if(isLeafIns){
				bool found=false;
				for(dfgNode* parent : node->getAncestors()){
					if(node->ancestorConditionaMap.find(parent)!=node->ancestorConditionaMap.end()){
						// the condition parent
						Instruction* selCondIns = cast<Instruction>(SLI->getCondition());
						LLVM_DEBUG(selCondIns->dump());
						dfgNode* selCond = findNode(selCondIns);
						assert(selCond != NULL);
						combineConditionAND(parent,selCond,node);
					}
				}
				DFG_CHECK(found,
					"handleSELECTNodes",
					"could not find conditional ancestor for leaf SELECT node " + std::to_string(node->getIdx()) +
					" (unsupported SELECT pattern in input)");
			}
			else{
				/// if its not a leaf instruction, I dont have to do anything

			}
			selectNodes.push_back(node);
		}
	}

	for(dfgNode* node : selectNodes){

		if(SelectInst* SLI = dyn_cast<SelectInst>(node->getNode())){

			std::vector<dfgNode*> mergeNodes;
			std::map<dfgNode*,std::pair<dfgNode*,bool>> mergeNodeControl;
			LLVM_DEBUG(SLI->getCondition()->dump());
			LLVM_DEBUG(SLI->getFalseValue()->dump());
			LLVM_DEBUG(SLI->getTrueValue()->dump());
			dfgNode* mergeNode;
			//Instruction* ins = dyn_cast<Instruction>(V)
			Instruction* ctrlins = dyn_cast<Instruction>(SLI->getCondition());
//			Instruction* false_datains = dyn_cast<Instruction>(SLI->getFalseValue());
			dfgNode* ctrl = findNode(ctrlins);
//			dfgNode* falsedata = findNode(false_datains);
//			node->removeAncestor(falsedata);
//			falsedata->removeChild(node);
			node->removeAncestor(ctrl);
			ctrl->removeChild(node);

//			mergeNode = insertMergeNodeBeforeSEL(node,ctrl,false,falsedata);
//			mergeNodes.push_back(mergeNode);
//			bool isBackEdge = false;
//			node->addAncestorNode(mergeNode,EDGE_TYPE_DATA,isBackEdge);
//			mergeNode->addChildNode(node,EDGE_TYPE_DATA,isBackEdge);

			//Original SELECT instruction has constant,remove the constant value from instruction and supply it from
			//CMERGE node
//			LLVM_DEBUG(dbgs() << SLI->getCondition()<<"\n");
//			LLVM_DEBUG(dbgs() << SLI->getTrueValue()<<"\n");
//			LLVM_DEBUG(dbgs() << SLI->getFalseValue()<<"\n");
			if(ConstantInt* CI = dyn_cast<ConstantInt>(SLI->getTrueValue())){

				LLVM_DEBUG(CI->dump());
				int constant = 0;
				if(CI){
					constant = CI->getSExtValue();
				}
				mergeNode = insertMergeNodeBeforeSEL(node,ctrl,true,constant);
				bool isBackEdge = false;
				node->addAncestorNode(mergeNode,EDGE_TYPE_DATA,isBackEdge);
				mergeNode->addChildNode(node,EDGE_TYPE_DATA,isBackEdge);

				node->removeConstantVal();
			}
			else{
				Instruction* true_datains = dyn_cast<Instruction>(SLI->getTrueValue());
				dfgNode* truedata = findNode(true_datains);
				node->removeAncestor(truedata);
				truedata->removeChild(node);

				mergeNode = insertMergeNodeBeforeSEL(node,ctrl,true,truedata);
				mergeNodes.push_back(mergeNode);
				bool isBackEdge = false;
				node->addAncestorNode(mergeNode,EDGE_TYPE_DATA,isBackEdge);
				mergeNode->addChildNode(node,EDGE_TYPE_DATA,isBackEdge);
			}


			if(ConstantInt* CI = dyn_cast<ConstantInt>(SLI->getFalseValue())){

				LLVM_DEBUG(CI->dump());
				int constant = 0;
				if(CI){
					constant = CI->getSExtValue();
				}
				mergeNode = insertMergeNodeBeforeSEL(node,ctrl,false,constant);
				bool isBackEdge = false;
				node->addAncestorNode(mergeNode,EDGE_TYPE_DATA,isBackEdge);
				mergeNode->addChildNode(node,EDGE_TYPE_DATA,isBackEdge);

				node->removeConstantVal();
			}
			else{
				Instruction* false_datains = dyn_cast<Instruction>(SLI->getFalseValue());
				dfgNode* falsedata = findNode(false_datains);
				node->removeAncestor(falsedata);
				falsedata->removeChild(node);

				mergeNode = insertMergeNodeBeforeSEL(node,ctrl,false,falsedata);
				mergeNodes.push_back(mergeNode);
				bool isBackEdge = false;
				node->addAncestorNode(mergeNode,EDGE_TYPE_DATA,isBackEdge);
				mergeNode->addChildNode(node,EDGE_TYPE_DATA,isBackEdge);
			}
			//node->removeChild(child);

		}

	}
	return 0;



}

dfgNode* DFGPartPred::combineConditionAND(dfgNode* brcond, dfgNode* selcond, dfgNode* child) {
	dfgNode* temp = new dfgNode(this);
	temp->setNameType("CMERGECOND");
	temp->setIdx(getNodesPtr()->size());
	temp->BB = child->BB;
	getNodesPtr()->push_back(temp);

	selcond->removeChild(child);
	child->removeAncestor(selcond);

	selcond->addChildNode(temp);
	temp->addAncestorNode(selcond);

	assert(child->ancestorConditionaMap.find(brcond)!=child->ancestorConditionaMap.end());
	bool brCondVal = (child->ancestorConditionaMap[brcond] == TRUE);
	bool isBrBackEdge = checkBackEdge(brcond,child);
	brcond->removeChild(child);
	child->removeAncestor(brcond);

	brcond->addChildNode(temp,EDGE_TYPE_DATA,isBrBackEdge,true,brCondVal);
	temp->addAncestorNode(brcond,EDGE_TYPE_DATA,isBrBackEdge,true,brCondVal);

	return temp;
}

bool DFGPartPred::checkBackEdge(dfgNode* src, dfgNode* dest) {
	//	dfgNode* firstnode = NodeList[0];
	//	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>,8> BackedgeBBs;
	//	FindFunctionBackedges(*(firstnode->getNode()->getFunction()),BackedgeBBs);
	//	const BasicBlock* srcBB = src->BB;
	//	const BasicBlock* destBB  = dest->BB;
	//
	//	std::pair<const BasicBlock *, const BasicBlock *> bbCuple;
	//	bbCuple = std::make_pair(srcBB,destBB);
	//	if(std::find(BackedgeBBs.begin(),BackedgeBBs.end(),bbCuple)!=BackedgeBBs.end()){
	//		return true;
	//	}
	//	else{
	//		return false;
	//	}
	const BasicBlock* srcBB = src->BB;
	const BasicBlock* destBB = dest->BB;

	//	if(srcBB == destBB) return true;

	if(std::find(BBSuccBasicBlocks[srcBB].begin(), BBSuccBasicBlocks[srcBB].end(), destBB) != BBSuccBasicBlocks[srcBB].end()){
		return false; //not a backedge
	}
	else{
		return true;
	}

}
