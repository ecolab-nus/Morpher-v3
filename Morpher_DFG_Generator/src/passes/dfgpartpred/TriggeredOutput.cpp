#include "DfgPartPredInternal.h"

void DFGPartPred::generateTrigDFGDOT(Function &F) {


	std::set<exitNode> exitNodes;

	getLoopExitConditionNodes(exitNodes);
	//	connectBB();
	removeAlloc();
	connectBBTrig();
	createCtrlBROrTree();
	if (!this->suppressDOT) printDOT(this->name + "bef_handlePHINodes_PartPredDFG.dot");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][handlePHINodes begin]\n");
	handlePHINodes(this->loopBB);

	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][handlePHINodes end]\n");
	if (!this->suppressDOT) printDOT(this->name + "after_handlePHINodes_PartPredDFG.dot");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][handleSELECTNodes begin]\n");
	handleSELECTNodes();

	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][handleSELECTNodes end]\n");
	if (!this->suppressDOT) printDOT(this->name + "after_handleSELECTNodes_PartPredDFG.dot");

	//exit(true);

	// insertshiftGEPs();
	addMaskLowBitInstructions();
	insertshiftGEPsCorrect();
	//removeDisconnetedNodes();
	//	scheduleCleanBackedges();
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][fillCMergeMutexNodes begin]\n");
	fillCMergeMutexNodes();
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][fillCMergeMutexNodes end]\n");


	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][constructCMERGETree begin]\n");
	constructCMERGETree();
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][constructCMERGETree end]\n");

	//	printDOT(this->name + "_PartPredDFG.dot"); return;
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][addLoopExitStoreHyCUBE begin]\n");
	addLoopExitStoreHyCUBE(exitNodes);
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][addLoopExitStoreHyCUBE end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][handlestartstop begin]\n");
	handlestartstop();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][handlestartstop end] Nodelist size:" <<NodeList.size()<<"\n\n");

	//	printDOT(this->name + "afterhandlestartstop_PartPredDFG.dot");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][scheduleASAP begin]\n");
	scheduleASAP();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][scheduleASAP end]\n\n");
	//	printDOT(this->name + "afterscheduleASAP_PartPredDFG.dot");
	//	return;
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][scheduleALAP begin]\n");
	scheduleALAP();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][scheduleALAP end]\n\n");
	// assignALAPasASAP();
	//	balanceSched();


	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][GEPBaseAddrCheck begin]\n");
	GEPBaseAddrCheck(F);
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][GEPBaseAddrCheck end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][nameNodes begin]\n");
	nameNodes();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][nameNodes end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][classifyParents begin]\n");
	classifyParents();
	// RemoveInductionControlLogic();

	// RemoveBackEdgePHIs();
	// removeOutLoopLoad();
	//	RemoveConstantCMERGEs(); Originally on morpher
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][classifyParents end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][removeDisconnectedNodes begin]\n");
	//removeDisconnectedNodes();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][removeDisconnectedNodes end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][addOrphanPseudoEdges begin]\n");
	addOrphanPseudoEdges();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][addOrphanPseudoEdges end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][addRecConnsAsPseudo begin]\n");
	addRecConnsAsPseudo();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][addRecConnsAsPseudo end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][changeTypeofSingleSourceCompNodes begin]\n");
	// printDOT(this->name + "_PartPredDFG.dot");
	// printNewDFGXML();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][changeTypeofSingleSourceCompNodes end]\n\n");
	LLVM_DEBUG(dbgs() << "\n[DFGPartPred.cpp][removeDisconnectedNodes begin]\n");


	//function removeDisconnetedNodes() should be called at last, otherwise same idx would be reused
	//when new instructions are added
	removeDisconnetedNodes();
	LLVM_DEBUG(dbgs() << "[DFGPartPred.cpp][removeDisconnectedNodes end]\n\n");
}

/*
 * From hycube branch
 *
void DFGPartPred::generateTrigDFGDOT(Function &F)
{

	//	connectBB();
	// std::pair<dfgNode *, bool> le_cond = getLoopExitConditionNode();

	std::set<exitNode> exitNodes;
	getLoopExitConditionNodes(exitNodes);

	// evaluateMUNITTrans();

	connectBBTrig();
	handlePHINodes(this->loopBB);

	createCtrlBROrTree();
	//	insertshiftGEPs();
	addMaskLowBitInstructions();
	insertshiftGEPsCorrect();
	removeDisconnetedNodes();
	//	scheduleCleanBackedges();
	fillCMergeMutexNodes();

	constructCMERGETree();

	//	printDOT(this->name + "_PartPredDFG.dot"); return;
	classifyParents();
	// addLoopExitStoreHyCUBE(le_cond);
	addLoopExitStoreHyCUBE(exitNodes);
	handlestartstop();

	scheduleASAP();
	scheduleALAP();
	//	assignALAPasASAP();
	//	balanceSched();
	//	removeOutLoopLoad();
	addOrphanPseudoEdges();

	GEPBaseAddrCheck(F);
	nameNodes();
}
 */

bool DFGPartPred::checkPHILoop(dfgNode* node1, dfgNode* node2) {
	if(node1->getNode()==NULL) return false;
	if(node2->getNode()==NULL) return false;

	if(PHINode* phi1 = dyn_cast<PHINode>(node1->getNode())){
		if(PHINode* phi2 = dyn_cast<PHINode>(node2->getNode())){
			bool is2isParentOf1 = false;
			bool is1isParentOf2 = false;
			for (int i1 = 0; i1 < phi1->getNumIncomingValues(); ++i1) {
				if(phi1->getIncomingValue(i1)==phi2){
					is2isParentOf1=true;
					break;
				}
			}
			for (int i2 = 0; i2 < phi2->getNumIncomingValues(); ++i2) {
				if(phi2->getIncomingValue(i2)==phi1){
					is1isParentOf2=true;
					break;
				}
			}
			if(is2isParentOf1 & is1isParentOf2) LLVM_DEBUG(dbgs() << "PHI LOOP FOUND!!!!!!\n");
			return is2isParentOf1 & is1isParentOf2;
		}
		else{
			return false;
		}
	}
	else{
		return false;
	}
}

bool DFGPartPred::checkBRPath(const BranchInst* BRParentBRI, const BasicBlock* currBB, bool& condVal,  std::set<const BasicBlock*>& searchedSoFar) {
	if(BRParentBRI->getSuccessor(0)==currBB){
		condVal=true;
		return true;
	}
	else if(BRParentBRI->getSuccessor(1)==currBB){
		condVal=false;
		return true;
	}
	else{
		searchedSoFar.insert(BRParentBRI->getParent());
		for (int succ = 0; succ < BRParentBRI->getNumSuccessors(); ++succ) {
			BasicBlock* succBB = BRParentBRI->getSuccessor(succ);
			if(searchedSoFar.find(succBB)==searchedSoFar.end()){
				BranchInst* succBRParentBRI = cast<BranchInst>(succBB->getTerminator());
				if(checkBRPath(succBRParentBRI,currBB,condVal,searchedSoFar)){
					return true;
				}
			}
		}

		LLVM_DEBUG(BRParentBRI->dump());
		LLVM_DEBUG(dbgs() << BRParentBRI->getSuccessor(0)->getName() << "\n");
		LLVM_DEBUG(dbgs() << BRParentBRI->getSuccessor(1)->getName() << "\n");
		LLVM_DEBUG(dbgs() << currBB->getName() << "\n");
		LLVM_DEBUG(BRParentBRI->dump());
		return false;
	}

}
