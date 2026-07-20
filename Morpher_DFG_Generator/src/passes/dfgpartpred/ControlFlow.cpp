#include "DfgPartPredInternal.h"

void DFGPartPred::connectBBTrig() {


	std::map<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> CtrlBBInfo = getCtrlInfoBBMorePaths();

	for(std::pair<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> p1 : CtrlBBInfo){
		BasicBlock* childBB = p1.first;
		//		std::vector<dfgNode*> leaves = getLeafs(childBB);
		std::vector<dfgNode*> leaves = getStoreInstructions(childBB);

		for(std::pair<BasicBlock*,CondVal> p2 : p1.second){
			BasicBlock* parentBB = p2.first;
			CondVal parentVal = p2.second;

			BranchInst* BRI = cast<BranchInst>(parentBB->getTerminator());
			bool isConditional = BRI->isConditional();
			assert(isConditional);

			dfgNode* BR = findNode(BRI); assert(BR);

			//			dfgNode* mergeNode;
			if(BR->getAncestors().size() != 1){
				assert(BR->getAncestors().size() == 2);
				LLVM_DEBUG(dbgs() << "Br=" << BR->getIdx() << ",Ins=");
				LLVM_DEBUG(BRI->dump());
				LLVM_DEBUG(dbgs() << "Ancestor Count=" << BR->getAncestors().size() << "\n");
				LLVM_DEBUG(dbgs() << "Ancestors=");
				for(dfgNode* par : BR->getAncestors()){
					LLVM_DEBUG(dbgs() << par->getIdx() << ",BB=" << par->BB->getName() <<":");
					if(par->getNode()) LLVM_DEBUG(par->getNode()->dump());
				}
			}
			assert(BR->getAncestors().size() == 1);
			BrParentMap[parentBB]=BR->getAncestors()[0];
			BrParentMapInv[BR->getAncestors()[0]]=parentBB;

			LLVM_DEBUG(dbgs() << "ConnectBB :: From=" << BrParentMap[parentBB]->getIdx() << "(" << parentBB->getName() << ")" << ",To=");
			for(dfgNode* succNode : leaves){
				LLVM_DEBUG(dbgs() << succNode->getIdx() << ",");
				assert(succNode->getNameType() != "OutLoopLOAD");
			}
			LLVM_DEBUG(dbgs() << ",destBB = " << childBB->getName());
			LLVM_DEBUG(dbgs() << "\n");

			for(dfgNode* BRParent : BR->getAncestors()){
				for(dfgNode* succNode : leaves){
					leafControlInputs[succNode].insert(BRParent);
					//				isBackEdge = checkBackEdge(node,succNode); //getCtrlInfoBB does not return backedges
					//					if(!succNode->isParent(BRParent)){
					BRParent->addChildNode(succNode,EDGE_TYPE_DATA,false,isConditional,(parentVal==TRUE));
					succNode->addAncestorNode(BRParent,EDGE_TYPE_DATA,false,isConditional,(parentVal==TRUE));
					//					}
				}
			}
		}
	}
}

void DFGPartPred::createCtrlBROrTree() {

	LLVM_DEBUG(dbgs() << "createCtrlBROrTree STARTED!\n");

	for(dfgNode* node : NodeList){
		std::set<dfgNode*> predicateParents;
		for(dfgNode* par : node->getAncestors()){
			if(par->childConditionalMap[node] != UNCOND){
				predicateParents.insert(par);
			}
		}

		if(predicateParents.size() > 1){
			std::queue<dfgNode*> q;
			for(dfgNode* pp : predicateParents){
				q.push(pp);
			}
			while(!q.empty()){
				dfgNode* pp1 = q.front(); q.pop();
				if(!q.empty()){
					dfgNode* pp2 = q.front(); q.pop();

					LLVM_DEBUG(dbgs() << "Connecting pp1 = " << pp1->getIdx() << ",");
					LLVM_DEBUG(dbgs() << "pp2 = " << pp2->getIdx() << ",");


					dfgNode* temp = new dfgNode(this);
					temp->setIdx(5000 + NodeList.size());
					temp->setNameType("CTRLBrOR");
					temp->BB = pp1->BB;
					NodeList.push_back(temp);

					LLVM_DEBUG(dbgs() << "newNode = " << temp->getIdx() << "\n");

					bool isPP1BackEdge = pp1->childBackEdgeMap[node];
					bool isPP2BackEdge = pp2->childBackEdgeMap[node];

					pp1->addChildNode(temp);
					temp->addAncestorNode(pp1);

					pp2->addChildNode(temp);
					temp->addAncestorNode(pp2);

					temp->addChildNode(node,EDGE_TYPE_DATA,isPP1BackEdge,true,pp1->childConditionalMap[node]);
					node->addAncestorNode(temp,EDGE_TYPE_DATA,isPP1BackEdge,true,pp1->childConditionalMap[node]);

					pp1->removeChild(node);
					node->removeAncestor(pp1);
					pp2->removeChild(node);
					node->removeAncestor(pp2);
					q.push(temp);
				}
				else{
					LLVM_DEBUG(dbgs() << "Alone node = " << pp1->getIdx() << "\n");
				}

			}
		}
	}

	LLVM_DEBUG(dbgs() << "createCtrlBROrTree ENDED!\n");

}

std::map<BasicBlock*, std::set<std::pair<BasicBlock*, CondVal> > > DFGPartPred::getCtrlInfoBBMorePaths() {


	//This function will return a map of basicblocks to their control dependent parents with the
	// respective control value

	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>,8> BackedgeBBs;
	dfgNode firstNode = *NodeList[0];
	FindFunctionBackedges(*(firstNode.getNode()->getFunction()),BackedgeBBs);

	std::map<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> res;

	for(BasicBlock* BB : loopBB){

		std::queue<BasicBlock*> q;
		q.push(BB);

		std::map<BasicBlock*,std::set<CondVal>> visited;

		while(!q.empty()){
			BasicBlock* curr = q.front(); q.pop();
			//			LLVM_DEBUG(dbgs() << "curr=" << curr->getName() << "\n";

			for (auto it = pred_begin(curr), et = pred_end(curr); it != et; ++it)
			{
				BasicBlock* predecessor = *it;
				if(loopBB.find(predecessor)==loopBB.end()){
					continue; // no need to care for out of loop BBs.
				}

				//		      if(predecessor == BB) continue;

				std::pair<const BasicBlock*,const BasicBlock*> bbPair = std::make_pair((const BasicBlock*)predecessor,(const BasicBlock*)curr);
				if(std::find(BackedgeBBs.begin(),BackedgeBBs.end(),bbPair)!=BackedgeBBs.end()){
					continue; // no need to traverse backedges;
				}

				CondVal cv;
				assert(predecessor->getTerminator());
				LLVM_DEBUG(predecessor->getTerminator()->dump());
				BranchInst* BRI = cast<BranchInst>(predecessor->getTerminator());
				//			  BRI->dump();

				if(!BRI->isConditional()){
					//				  LLVM_DEBUG(dbgs() << "\t0 :: ";
					cv = UNCOND;
				}
				else{
					for (int i = 0; i < BRI->getNumSuccessors(); ++i) {
						if(BRI->getSuccessor(i) == curr){
							if(i==0){
								//							  LLVM_DEBUG(dbgs() << "\t1 :: ";
								cv = TRUE;
							}
							else if(i==1){
								//							  LLVM_DEBUG(dbgs() << "\t2 :: ";
								cv = FALSE;
							}
							else{
								DFG_CHECK(false, "getDependentCtrlBBs",
									"branch has more than 2 successors (switch statements are not supported)");
							}
						}
					}
				}

				//			  LLVM_DEBUG(dbgs() << "\tPred=" << predecessor->getName() << "(" << dfgNode::getCondValStr(cv) << ")\n";

				visited[predecessor].insert(cv);
				q.push(predecessor);
			}
		}

		LLVM_DEBUG(dbgs() << "BasicBlock : " << BB->getName() << " :: DependentCtrlBBs = ");
		res[BB];
		for(std::pair<BasicBlock*,std::set<CondVal>> pair : visited){
			BasicBlock* bb = pair.first;
			std::set<CondVal> brOuts = pair.second;


			//			if(brOuts.size() == 1){
			//				if(brOuts.find(UNCOND) == brOuts.end()){
			LLVM_DEBUG(dbgs() << bb->getName());
			//					if(brOuts.find(UNCOND) != brOuts.end()){
			//						res[BB].insert(std::make_pair(bb,UNCOND));
			//						LLVM_DEBUG(dbgs() << "(UNCOND),";
			//					}
			if(brOuts.find(TRUE) != brOuts.end()){
				res[BB].insert(std::make_pair(bb,TRUE));
				LLVM_DEBUG(dbgs() << "(TRUE),");
			}
			if(brOuts.find(FALSE) != brOuts.end()){
				res[BB].insert(std::make_pair(bb,FALSE));
				LLVM_DEBUG(dbgs() << "(FALSE),");
			}
			//					res[BB].insert(std::make_pair(bb,cv));
			//				}
			//			}
		}
		LLVM_DEBUG(dbgs() << "\n");
	}

	LLVM_DEBUG(dbgs() << "###########################################################################\n");

	std::map<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> temp1;

	for(std::pair<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> pair : res){
		std::set<std::pair<BasicBlock*,CondVal>> tobeRemoved;
		BasicBlock* currBB = pair.first;
		LLVM_DEBUG(dbgs() << "BasicBlock : " << currBB->getName() << " :: DependentCtrlBBs = ");

		temp1[currBB];
		for(std::pair<BasicBlock*,CondVal> bbVal : pair.second){
			BasicBlock* depBB = bbVal.first;
			for(std::pair<BasicBlock*,CondVal> p2 : res[depBB]){
				tobeRemoved.insert(p2);
			}
		}

		for(std::pair<BasicBlock*,CondVal> bbVal : pair.second){
			if(tobeRemoved.find(bbVal)==tobeRemoved.end()){
				LLVM_DEBUG(dbgs() << bbVal.first->getName());
				LLVM_DEBUG(dbgs() << "(" << dfgNode::getCondValStr(bbVal.second) << "),");
				temp1[currBB].insert(bbVal);
			}
		}
		LLVM_DEBUG(dbgs() << "\n");
	}
	//	return temp1;

	std::map<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> temp2;

	LLVM_DEBUG(dbgs() << "Order=");
	for(std::pair<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> pair : temp1){
		BasicBlock* currBB = pair.first;
		LLVM_DEBUG(dbgs() << currBB->getName() << ",");
		std::set<std::pair<BasicBlock*,CondVal>> bbValPairs = pair.second;

		std::queue<std::pair<BasicBlock*,CondVal>> bbValQ;
		for(std::pair<BasicBlock*,CondVal> bbVal: bbValPairs){
			bbValQ.push(bbVal);
		}

		bbValPairs.clear();
		int rounds = 0;
		while(!bbValQ.empty()){
			if(rounds == bbValQ.size()*2) break;
			std::pair<BasicBlock*,CondVal> topBBVal1 = bbValQ.front(); bbValQ.pop();
			if(!bbValQ.empty()){
				std::pair<BasicBlock*,CondVal> topBBVal2 = bbValQ.front();
				if(topBBVal1.first == topBBVal2.first){
					rounds=0;
					bbValQ.pop();
					if(!temp2[topBBVal1.first].empty()){
						bbValQ.push(*temp2[topBBVal1.first].rbegin());
					}
				}
				else{
					bbValQ.push(topBBVal1);
					rounds++;
					//					break;
				}
			}else{
				bbValPairs.insert(topBBVal1);
			}
		}

		while(!bbValQ.empty()){
			bbValPairs.insert(bbValQ.front()); bbValQ.pop();
		}

		if(!bbValPairs.empty()) temp2[currBB]=bbValPairs;
	}
	LLVM_DEBUG(dbgs() << "\n");

	LLVM_DEBUG(dbgs() << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n");

	temp1.clear();
	for(std::pair<BasicBlock*,std::set<std::pair<BasicBlock*,CondVal>>> pair : temp2){
		BasicBlock* currBB = pair.first;
		std::set<std::pair<BasicBlock*,CondVal>> bbValPairs = pair.second;
		LLVM_DEBUG(dbgs() << "BasicBlock : " << currBB->getName() << " :: DependentCtrlBBs = ");
		for(std::pair<BasicBlock*,CondVal> bbVal: bbValPairs){
			LLVM_DEBUG(dbgs() << bbVal.first->getName());
			LLVM_DEBUG(dbgs() << "(" << dfgNode::getCondValStr(bbVal.second) << "),");
		}

		if(!bbValPairs.empty()){
			temp1[currBB] = temp2[currBB];
		}
		else{
			LLVM_DEBUG(dbgs() << "Not added!");
		}
		LLVM_DEBUG(dbgs() << "\n");
	}
	//	assert(false);
	return temp1;

}

int findParentClassification(dfgNode* par, dfgNode* child){

	if(child->parentClassification[0]==par){
		return 0;
	}
	else if(child->parentClassification[1]==par){
		return 1;
	}
	else if(child->parentClassification[2]==par){
		return 2;
	}

	return -1;
}



void DFGPartPred::RemoveInductionControlLogic() {

	// ScalarEvolution* SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();

	PHINode* ind_phi = currLoop->getInductionVariable(*SE);
	DFG_CHECK(ind_phi,
		"RemoveInductionControlLogic",
		"cannot find the induction variable; the loop induction variable increments may not be 1");

	dfgNode* ind_phi_node = findNode(ind_phi);
	assert(ind_phi_node);

	std::vector<dfgNode*> phiChildren = ind_phi_node->getChildren();
	std::map<dfgNode*,int> phiChildParIdxMap;

	//Ind phi node should have two parents
	// assert(ind_phi_node->getAncestors().size() == 2);
	int anc_count = 0;
	for(dfgNode* par : ind_phi_node->getAncestors()){
		Edge* e = findEdge(par,ind_phi_node);
		if(e->getType() == EDGE_TYPE_PS){
			continue;
		}
		LLVM_DEBUG(dbgs() << "induction phi parents :: \n");
		LLVM_DEBUG(dbgs() << par->getIdx() << ",");
		anc_count++;
	}
	assert(anc_count == 2);


	dfgNode* ind_phi_init_par = NULL;
	dfgNode* ctrlNode = NULL;
	dfgNode* dataNode = NULL;
	dfgNode* cmerge = NULL;

	for(dfgNode* par : ind_phi_node->getAncestors()){
		if(ind_phi_node->ancestorBackEdgeMap[par]){

			//backedge cmerge
			assert(par->getNameType() == "CMERGE");
			cmerge = par;

			ctrlNode = cmergeCtrlInputs[par];  assert(ctrlNode);
			dataNode = cmergeDataInputs[par];	 assert(dataNode);

			std::vector<dfgNode*> dParents = dataNode->getAncestors();

			//Parent of the data of cmerge should be phi
			assert(std::find(dParents.begin(),dParents.end(),ind_phi_node) != dParents.end());

			//removing parents of ctrl
			for(dfgNode* cPar : ctrlNode->getAncestors()){
				cPar->removeChild(ctrlNode);
				ctrlNode->removeAncestor(cPar);
			}

			//removing parents of cmerge
			for(dfgNode* cmPar : par->getAncestors()){
				cmPar->removeChild(par);
				par->removeAncestor(cmPar);
			}
		}
		else{
			ind_phi_init_par = par;
			ind_phi_init_par->removeChild(ind_phi_node);
			ind_phi_node->removeAncestor(ind_phi_init_par);
		}
	}

	for(dfgNode* phiC : ind_phi_node->getChildren()){
		int par_class = findParentClassification(ind_phi_node,phiC);
		assert(par_class!=-1);
		phiChildParIdxMap[phiC]=par_class;

		ind_phi_node->removeChild(phiC);
		phiC->removeAncestor(ind_phi_node);
	}

	assert(ctrlNode);
	assert(dataNode);
	assert(ind_phi_init_par);
	assert(cmerge);

	//Adding new connections
	ind_phi_init_par->addChildNode(dataNode,EDGE_TYPE_DATA);
	dataNode->addAncestorNode(ind_phi_init_par,EDGE_TYPE_DATA);

	int phi_init_class = findParentClassification(ind_phi_init_par,ind_phi_node);
	assert(phi_init_class != -1);
	dataNode->parentClassification[phi_init_class]=ind_phi_init_par;

	//add its own self as a recurrent edge
	//FIXME : hardcoding the recurrent edge as parent 2
	dataNode->addChildNode(dataNode,EDGE_TYPE_DATA,true);
	dataNode->addAncestorNode(dataNode,EDGE_TYPE_DATA,true);
	Edge2OperandIdxMap[dataNode][dataNode]=phi_init_class;

	for(dfgNode* phiChild : phiChildren){
		if(phiChild == dataNode) continue;
		dataNode->addChildNode(phiChild,EDGE_TYPE_DATA);
		phiChild->addAncestorNode(dataNode,EDGE_TYPE_DATA);
		phiChild->parentClassification[phiChildParIdxMap[phiChild]]=dataNode;
	}



	//remove nodes
	NodeList.erase(std::remove(NodeList.begin(), NodeList.end(), cmerge), NodeList.end());
	NodeList.erase(std::remove(NodeList.begin(), NodeList.end(), ind_phi_node), NodeList.end());
}


void DFGPartPred::RemoveBackEdgePHIs() {
	LLVM_DEBUG(dbgs() << "removebackedge phis begin...\n");

	for(dfgNode* n : NodeList){
		if(n->getNode()){
			if(PHINode* phiNode = dyn_cast<PHINode>(n->getNode())){

				if(n->getAncestors().size() != 2) continue;

				LLVM_DEBUG(dbgs()  << "processing phi node = " << n->getIdx());

				dfgNode* normal_anc = NULL;
				dfgNode* backedge_anc = NULL;

				for(dfgNode* anc : n->getAncestors()){
					if(n->ancestorBackEdgeMap[anc]){
						assert(backedge_anc == NULL);
						backedge_anc = anc;
					}
					else{
						assert(normal_anc == NULL);
						normal_anc = anc;
					}
				}

				LLVM_DEBUG(dbgs()  << ",BE_anc = " << backedge_anc->getIdx() << ",normal_anc = " << normal_anc->getIdx() << "\n");

				//process non-back edge node
				assert(normal_anc->getNameType() == "CMERGE");
				if(normal_anc->hasConstantVal()){
					for(dfgNode* phiC : n->getChildren()){

						LLVM_DEBUG(dbgs()  << "adding child:" << phiC->getIdx() << ",to:" << normal_anc->getIdx() << "\n");
						normal_anc->addChildNode(phiC,EDGE_TYPE_DATA);
						phiC->addAncestorNode(normal_anc,EDGE_TYPE_DATA);
						Edge2OperandIdxMap[normal_anc][phiC] = findParentClassification(n,phiC);

						if(phiC->getNameType() == "CMERGE") cmergeDataInputs[phiC]=normal_anc;

						// phiC->parentClassification[findParentClassification(n,phiC)]=normal_anc;
					}
					normal_anc->removeChild(n);
					n->removeAncestor(normal_anc);
				}
				else{
					dfgNode* ctrlNode = cmergeCtrlInputs[normal_anc]; assert(ctrlNode);
					// dfgNode* dataNode = cmergeDataInputs[normal_anc]; assert(dataNode);

					// dfgNode* ctrlNode = normal_anc->parentClassification[0]; assert(ctrlNode);
					// dfgNode* dataNode = dataNode->parentClassification[1]; assert(dataNode);

					vector<dfgNode*> dataParents = normal_anc->getAncestors();
					for(dfgNode* dataParent : dataParents){
						if(dataParent == ctrlNode) continue;
						if(findEdge(dataParent,normal_anc)->getType() == EDGE_TYPE_PS) continue;
						bool isBackEdge = dataParent->childBackEdgeMap[normal_anc];

						for(dfgNode* phiC : n->getChildren()){
							LLVM_DEBUG(dbgs()   << "adding child:" << phiC->getIdx() << ",to:" << dataParent->getIdx() << "\n");
							dataParent->addChildNode(phiC,EDGE_TYPE_DATA,isBackEdge);
							phiC->addAncestorNode(dataParent,EDGE_TYPE_DATA,isBackEdge);
							Edge2OperandIdxMap[dataParent][phiC] = findParentClassification(n,phiC);

							if(phiC->getNameType() == "CMERGE") cmergeDataInputs[phiC]=dataParent;

							dataParent->removeChild(normal_anc);
							normal_anc->removeAncestor(dataParent);

							// phiC->parentClassification[findParentClassification(n,phiC)]=dataNode;
						}
					}



					// dataNode->removeChild(normal_anc);
					// normal_anc->removeAncestor(dataNode);

					ctrlNode->removeChild(normal_anc);
					normal_anc->removeAncestor(ctrlNode);

					normal_anc->removeChild(n);
					n->removeAncestor(normal_anc);
				}

				//process backedge
				if(backedge_anc->hasConstantVal()){
					for(dfgNode* phiC : n->getChildren()){

						LLVM_DEBUG(dbgs()  << "adding (with backedge) child:" << phiC->getIdx() << ",to:" << backedge_anc->getIdx() << "\n");
						backedge_anc->addChildNode(phiC,EDGE_TYPE_DATA,true);
						phiC->addAncestorNode(backedge_anc,EDGE_TYPE_DATA,true);
						Edge2OperandIdxMap[backedge_anc][phiC] = findParentClassification(n,phiC);

						if(phiC->getNameType() == "CMERGE") cmergeDataInputs[phiC]=backedge_anc;
						// phiC->parentClassification[findParentClassification(n,phiC)]=backedge_anc;

						std::unordered_set<dfgNode*> lineage = getLineage(backedge_anc);
						if(lineage.find(phiC) == lineage.end() && !backedge_anc->getChildren().empty()){
							std::vector<dfgNode*> dChlds = backedge_anc->getChildren();
							dfgNode* nonbedge_child = NULL;
							for(dfgNode* chld : dChlds){
								std::vector<dfgNode*> next_childs = chld->getChildren();
								if(!backedge_anc->childBackEdgeMap[chld] &&
										std::find(next_childs.begin(),next_childs.end(),phiC) == next_childs.end()){
									nonbedge_child = chld; break;
								}
							}

							if(nonbedge_child){
								LLVM_DEBUG(dbgs()  << "adding (with pseudo) child:" << phiC->getIdx() << ",to:" << nonbedge_child->getIdx() << "\n");
								nonbedge_child->addChildNode(phiC,EDGE_TYPE_PS,false,true,true);
								phiC->addAncestorNode(nonbedge_child,EDGE_TYPE_PS,false,true,true);
							}

						}

					}
					backedge_anc->removeChild(n);
					n->removeAncestor(backedge_anc);
				}
				else{
					dfgNode* ctrlNode = cmergeCtrlInputs[backedge_anc]; assert(ctrlNode);
					// dfgNode* dataNode = cmergeDataInputs[backedge_anc]; assert(dataNode);

					vector<dfgNode*> dataParents = backedge_anc->getAncestors();
					for(dfgNode* dataParent : dataParents){
						if(dataParent == ctrlNode) continue;
						if(findEdge(dataParent,backedge_anc)->getType() == EDGE_TYPE_PS) continue;

						bool isBackEdge = dataParent->childBackEdgeMap[normal_anc];
						for(dfgNode* phiC : n->getChildren()){
							LLVM_DEBUG(dbgs()  << "adding (with backedge) child:" << phiC->getIdx() << ",to:" << dataParent->getIdx() << "\n");
							dataParent->addChildNode(phiC,EDGE_TYPE_DATA,true);
							phiC->addAncestorNode(dataParent,EDGE_TYPE_DATA,true);
							Edge2OperandIdxMap[dataParent][phiC] = findParentClassification(n,phiC);

							if(phiC->getNameType() == "CMERGE") cmergeDataInputs[phiC]=dataParent;
							// phiC->parentClassification[findParentClassification(n,phiC)]=dataNode;

							std::unordered_set<dfgNode*> lineage = getLineage(dataParent);
							if(lineage.find(phiC) == lineage.end() && !dataParent->getChildren().empty()){
								std::vector<dfgNode*> dChlds = dataParent->getChildren();
								dfgNode* nonbedge_child = NULL;
								for(dfgNode* chld : dChlds){
									std::vector<dfgNode*> next_childs = chld->getChildren();
									if(!dataParent->childBackEdgeMap[chld] &&
											std::find(next_childs.begin(),next_childs.end(),phiC) == next_childs.end()){
										nonbedge_child = chld; break;
									}
								}

								if(nonbedge_child){
									LLVM_DEBUG(dbgs()  << "adding (with pseudo) child:" << phiC->getIdx() << ",to:" << nonbedge_child->getIdx() << "\n");
									nonbedge_child->addChildNode(phiC,EDGE_TYPE_PS,false,true,true);
									phiC->addAncestorNode(nonbedge_child,EDGE_TYPE_PS,false,true,true);
								}

							}
						}

						dataParent->removeChild(backedge_anc);
						backedge_anc->removeAncestor(dataParent);

					}


					// dfgNode* ctrlNode = normal_anc->parentClassification[0]; assert(ctrlNode);
					// dfgNode* dataNode = dataNode->parentClassification[1]; assert(dataNode);



					// dataNode->removeChild(backedge_anc);
					// backedge_anc->removeAncestor(dataNode);

					ctrlNode->removeChild(backedge_anc);
					backedge_anc->removeAncestor(ctrlNode);

					backedge_anc->removeChild(n);
					n->removeAncestor(backedge_anc);
				}

				for(dfgNode* phiC : n->getChildren()){
					n->removeChild(phiC);
					phiC->removeAncestor(n);
				}


			}
		}
	}

	LLVM_DEBUG(dbgs()  << "removebackedge phis done.!\n");
}

void DFGPartPred::RemoveConstantCMERGEs(){

	for(dfgNode* n : NodeList){
		if(n->getNameType() == "CMERGE" && n->hasConstantVal()){
			dfgNode* ctrlNode = cmergeCtrlInputs[n];
			bool ctrlVal = n->ancestorConditionaMap[ctrlNode];

			vector<dfgNode*> cmChilds = n->getChildren();

			for(dfgNode* c : cmChilds){

				bool onlyParentCMERGE=true;
				for(dfgNode* par : c->getAncestors()){
					if(par != n && !c->ancestorBackEdgeMap[par]){
						onlyParentCMERGE=false;
					}
				}

				if(onlyParentCMERGE){
					ctrlNode->addChildNode(c,EDGE_TYPE_CTRL,false,true,ctrlVal);
					c->addAncestorNode(ctrlNode,EDGE_TYPE_CTRL,false,true,ctrlVal);
					Edge2OperandIdxMap[ctrlNode][c]=0;
				}

				n->removeChild(c);
				c->removeAncestor(n);
			}

			ctrlNode->removeChild(n);
			n->removeAncestor(ctrlNode);
		}
	}


}


void DFGPartPred::getLoopExitConditionNodes(std::set<exitNode> &exitNodes)
{
	bool is_ctrl_node_null = false;
	std::unordered_set<BasicBlock *> exitSrcs;

	for (auto it = loopexitBB.begin(); it != loopexitBB.end(); it++)
	{

		BasicBlock *src = it->first;
		BasicBlock *dest = it->second;
		exitSrcs.insert(src);

		LLVM_DEBUG(dbgs() << "loop exit src = " << src->getName() << ", dest = " << dest->getName() << ",");

		BranchInst *BRI = static_cast<BranchInst *>(src->getTerminator());

		if (BRI->isConditional())
		{
			dfgNode *ctrlNode = NULL;
			ConstantInt *ConstCond = dyn_cast<ConstantInt>(BRI->getCondition());
			if (!ConstCond)
			{
				ctrlNode = findNode(cast<Instruction>(BRI->getCondition()));
				assert(ctrlNode);
			}
			for (int j = 0; j < BRI->getNumSuccessors(); ++j)
			{
				if (dest == BRI->getSuccessor(j))
				{
					if (ConstCond && ConstCond->isZero() != (j == 1))
					{
						continue;
					}
					if (j == 0)
					{
						LLVM_DEBUG(dbgs() << "true path\n");
						// exitNodes.insert(std::make_pair(ctrlNode, true));
						exitNodes.insert(exitNode(ctrlNode, true, BRI->getParent(), dest));
					}
					else
					{
						LLVM_DEBUG(dbgs() << "false path\n");
						exitNodes.insert(exitNode(ctrlNode, false, BRI->getParent(), dest));
					}
				}
			}
		}
		else
		{
			LLVM_DEBUG(dbgs() << "BRI BasicBlock = " << BRI->getParent()->getName() << "\n");

			auto ThisBasicBlockDTNode = DT->getNode(BRI->getParent());
			auto IDOM_DTNode = ThisBasicBlockDTNode ? ThisBasicBlockDTNode->getIDom() : nullptr;
			BranchInst *IDOM_BRI = NULL;
			while (IDOM_DTNode)
			{
				BasicBlock *IDOM = IDOM_DTNode->getBlock();
				IDOM_BRI = dyn_cast<BranchInst>(IDOM->getTerminator());
				if (IDOM_BRI && IDOM_BRI->isConditional())
					break;
				IDOM_DTNode = IDOM_DTNode->getIDom();
			}

			if (!IDOM_BRI || !IDOM_BRI->isConditional())
			{
				exitNodes.insert(exitNode(NULL, true, BRI->getParent(), dest));
				is_ctrl_node_null = true;
				continue;
			}

			LLVM_DEBUG(dbgs() << "IDOM BRI BasicBlock = " << IDOM_BRI->getParent()->getName() << "\n");

			bool isTruePath = true;
			for (int j = 0; j < IDOM_BRI->getNumSuccessors(); ++j)
			{
				if (DT->dominates(IDOM_BRI->getSuccessor(j), BRI->getParent()))
				{
					if (j == 0)
					{
						LLVM_DEBUG(dbgs() << "true path\n");
						// exitNodes.insert(std::make_pair(ctrlNode, true));
						isTruePath = true;
					}
					else
					{
						LLVM_DEBUG(dbgs() << "false path\n");
						isTruePath = false;
					}
				}
			}

			dfgNode *ctrlNode = NULL;
			ConstantInt *ConstCond = dyn_cast<ConstantInt>(IDOM_BRI->getCondition());
			if (!ConstCond)
				ctrlNode = findNode(cast<Instruction>(IDOM_BRI->getCondition()));
			else if (ConstCond->isZero() == isTruePath)
				continue;

			//ctrlNode could be null that means it is always executed
			exitNodes.insert(exitNode(ctrlNode, isTruePath, BRI->getParent(), dest));

			if (ctrlNode)
			{
			}
			else
			{
				is_ctrl_node_null = true;
			}
		}
	}

	if (is_ctrl_node_null)
	{
		assert(exitSrcs.size() == 1);
	}
}

// void DFGPartPred::addLoopExitStoreHyCUBE(std::set<exitNode> &exitNodes)
// {
// 	LLVM_DEBUG(dbgs() << "addLoopExitStoreHyCUBE begin.\n";
// 	for (exitNode en : exitNodes)
// 	{

// 		dfgNode *loopexit = new dfgNode(this);
// 		loopexit->setIdx(this->getNodesPtr()->size() + 20000);
// 		this->getNodesPtr()->push_back(loopexit);
// 		loopexit->setNameType("LOOPEXIT");
// 		loopexit->BB = en.dest;
// 		loopexit->setLeftAlignedMemOp(1);

// 		dfgNode *mov_const = new dfgNode(this);
// 		mov_const->setIdx(this->getNodesPtr()->size() + 20000);
// 		this->getNodesPtr()->push_back(mov_const);
// 		mov_const->setNameType("MOVC");
// 		mov_const->BB = loopexit->BB;

// 		int mu_trans_id = getMUnitTransID(en.src, en.dest);
// 		mov_const->setConstantVal(mu_trans_id);

// 		mov_const->addChildNode(loopexit);
// 		loopexit->addAncestorNode(mov_const);
// 		loopexit->parentClassification[1] = mov_const;
// 		LLVM_DEBUG(dbgs() << "Adding connection : " << mov_const->getIdx() << " to " << loopexit->getIdx() << "\n";

// 		if (en.ctrlNode)
// 		{
// 			en.ctrlNode->getNode()->dump();
// 			bool isTruePath = en.ctrlVal;
// 			en.ctrlNode->addChildNode(loopexit, EDGE_TYPE_DATA, false, true, isTruePath);
// 			loopexit->addAncestorNode(en.ctrlNode, EDGE_TYPE_DATA, false, true, isTruePath);
// 			loopexit->parentClassification[0] = en.ctrlNode;
// 			if (!isTruePath)
// 				loopexit->setNPB(true);
// 			LLVM_DEBUG(dbgs() << "Adding connection : " << en.ctrlNode->getIdx() << " to " << loopexit->getIdx() << "\n";
// 		}
// 		else
// 		{
// 			dfgNode *sample_start_node = startNodes.begin()->second;
// 			assert(sample_start_node);
// 			sample_start_node->addChildNode(loopexit, EDGE_TYPE_PS, false, true, true);
// 			loopexit->addAncestorNode(sample_start_node, EDGE_TYPE_PS, false, true, true);

// 			LLVM_DEBUG(dbgs() << "Adding connection PS : " << sample_start_node->getIdx() << " to " << loopexit->getIdx() << "\n";
// 		}
// 	}

// 	LLVM_DEBUG(dbgs() << "addLoopExitStoreHyCUBE end.\n";

// 	// for(auto it = exitNodes.begin(); it != exitNodes.end(); it++){
// 	// 	dfgNode* control_node = it->first;
// 	// 	bool isTruePath = it->second;

// 	// 	node->addChildNode(loopexit,EDGE_TYPE_DATA,false,true,isTruePath);
// 	// 	loopexit->addAncestorNode(node,EDGE_TYPE_DATA,false,true,isTruePath);

// 	// 	//There is a problem here, we cannot have multiple nodes coming to the same store
// 	// 	loopexit->parentClassification[0] = node;
// 	// 	if(!isTruePath) loopexit->setNPB(true);
// 	// }
// }

void DFGPartPred::addLoopExitStoreHyCUBE(std::set<exitNode> &exitNodes)
{
	LLVM_DEBUG(dbgs() << "addLoopExitStoreHyCUBE begin.\n");
	for (exitNode en : exitNodes)
	{

		dfgNode *loopexit = new dfgNode(this);
		loopexit->setIdx(this->getNodesPtr()->size() + 20000);
		this->getNodesPtr()->push_back(loopexit);
		loopexit->setNameType("LOOPEXIT");
		loopexit->BB = en.dest;
		loopexit->setLeftAlignedMemOp(1);

		dfgNode *mov_const = new dfgNode(this);
		mov_const->setIdx(this->getNodesPtr()->size() + 20000);
		this->getNodesPtr()->push_back(mov_const);
		mov_const->setNameType("MOVC");
		mov_const->BB = loopexit->BB;

		int mu_trans_id = 1;//getMUnitTransID(en.src, en.dest);
		mov_const->setConstantVal(mu_trans_id);

		mov_const->addChildNode(loopexit);
		loopexit->addAncestorNode(mov_const);
		loopexit->parentClassification[1] = mov_const;
		LLVM_DEBUG(dbgs() << "Adding connection : " << mov_const->getIdx() << " to " << loopexit->getIdx() << "\n");

		if (en.ctrlNode)
		{
			LLVM_DEBUG(en.ctrlNode->getNode()->dump());
			bool isTruePath = en.ctrlVal;
			en.ctrlNode->addChildNode(loopexit, EDGE_TYPE_DATA, false, true, isTruePath);
			loopexit->addAncestorNode(en.ctrlNode, EDGE_TYPE_DATA, false, true, isTruePath);
			loopexit->parentClassification[0] = en.ctrlNode;
			if (!isTruePath)
				loopexit->setNPB(true);
			LLVM_DEBUG(dbgs() << "Adding connection : " << en.ctrlNode->getIdx() << " to " << loopexit->getIdx() << "\n");
		}
		else
		{
			dfgNode *sample_start_node = startNodes.begin()->second;
			assert(sample_start_node);
			sample_start_node->addChildNode(loopexit, EDGE_TYPE_PS, false, true, true);
			loopexit->addAncestorNode(sample_start_node, EDGE_TYPE_PS, false, true, true);

			LLVM_DEBUG(dbgs() << "Adding connection PS : " << sample_start_node->getIdx() << " to " << loopexit->getIdx() << "\n");
		}
	}

	LLVM_DEBUG(dbgs() << "addLoopExitStoreHyCUBE end.\n");

	// for(auto it = exitNodes.begin(); it != exitNodes.end(); it++){
	// 	dfgNode* control_node = it->first;
	// 	bool isTruePath = it->second;

	// 	node->addChildNode(loopexit,EDGE_TYPE_DATA,false,true,isTruePath);
	// 	loopexit->addAncestorNode(node,EDGE_TYPE_DATA,false,true,isTruePath);

	// 	//There is a problem here, we cannot have multiple nodes coming to the same store
	// 	loopexit->parentClassification[0] = node;
	// 	if(!isTruePath) loopexit->setNPB(true);
	// }
}
