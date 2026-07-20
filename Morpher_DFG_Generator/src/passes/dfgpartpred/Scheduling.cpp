#include "DfgPartPredInternal.h"

void DFGPartPred::scheduleCleanBackedges() {

	std::queue<dfgNode*> q;
	std::set<dfgNode*> visited;

	for(std::pair<BasicBlock*,dfgNode*> pair : startNodes){
		q.push(pair.second);
	}

	assert(q.size() == 1);

	while(!q.empty()){
		dfgNode* nFront = q.front(); q.pop();
		for(dfgNode* child : nFront->getChildren()){
			if(visited.find(child) != visited.end()){ // already visited, should be a backedge
				nFront->setBackEdge(child,true);
			}
			else{ // should not be a backedge
				nFront->setBackEdge(child,false);
				q.push(child);
			}
		}

		visited.insert(nFront);
	}

}
/*
 *
 *
 * */

void DFGPartPred::fillCMergeMutexNodes() {

	for(dfgNode* node : NodeList){
		std::set<dfgNode*> currMutexSet;
		if(node->getNode()!=NULL){
			if(SelectInst* SLI = dyn_cast<SelectInst>(node->getNode())){
				LLVM_DEBUG( dbgs()<< "SELECT instruction. No need to add SELECTPHI instruction through constructCMERGETree function\n") ;
				continue;
			}
		}
		for(dfgNode* parent : node->getAncestors()){
			if(parent->getNameType() == "CMERGE"){
				currMutexSet.insert(parent);
			}
		}

		if(!currMutexSet.empty()) mutexSets.insert(currMutexSet);
		mutexSetCommonChildren[currMutexSet].insert(node);

		for(dfgNode* m1 : currMutexSet){
			for(dfgNode* m2 : currMutexSet){
				if(m1 == m2) continue;
				mutexNodes[m1].insert(m2);
			}
		}
	}

}

/* If a node has two CMERGE instruction as parents, this function add SELECTPHI instruction between that node and
 * two CMERGE instructions.
 *
 */
void DFGPartPred::constructCMERGETree() {
	// assert(!mutexSets.empty());

	//if there are no mutex sets no point creating the cmerge tree
	if(mutexSets.empty()) return;

	for(std::set<dfgNode*> cmergeSet : mutexSets){
		std::queue<dfgNode*> thisIterSet;
		std::queue<dfgNode*> nextIterSet;

		LLVM_DEBUG(dbgs() << "CMERGE(ThisIter) set =");
		for(dfgNode* cmergeNode : cmergeSet){
			bool isBackEdge=false;
			LLVM_DEBUG(dbgs() << "CMERGE Tree children = ");
			for(dfgNode* child : mutexSetCommonChildren[cmergeSet]){
				LLVM_DEBUG(dbgs() << child->getIdx());
				if(isBackEdge) assert(cmergeNode->childBackEdgeMap[child] == true);
				if(cmergeNode->childBackEdgeMap[child]){
					isBackEdge = true;
					LLVM_DEBUG(dbgs() << "[BE]");
				}
				LLVM_DEBUG(dbgs() << ",");
			}
			LLVM_DEBUG(dbgs() << "\n");

			if(mutexSetCommonChildren[cmergeSet].size() == 1){
				dfgNode* singleChild = *mutexSetCommonChildren[cmergeSet].begin();
				if(singleChild->getNode() && (dyn_cast<PHINode>(singleChild->getNode()) || dyn_cast<SelectInst>(singleChild->getNode()))){
					realphi_as_selectphi.insert(singleChild);
					LLVM_DEBUG(dbgs() << "Only single child is a phi node, hence marking it as a SELECTPHI\n");
				}
			}

			if(isBackEdge){
				selectPHIAncestorMap[cmergeNode]=cmergePHINodes[cmergeNode];
				nextIterSet.push(cmergeNode);
			}
			else{
				LLVM_DEBUG(dbgs() << cmergeNode->getIdx() << ",");
				selectPHIAncestorMap[cmergeNode]=cmergePHINodes[cmergeNode];
				thisIterSet.push(cmergeNode);
			}
		}
		LLVM_DEBUG(dbgs() << "\n");

		if(thisIterSet.size() + nextIterSet.size() <= 1) continue;

		LLVM_DEBUG(dbgs() << "This ITER connections CMERGE Tree.\n");
		while(!thisIterSet.empty()){
			dfgNode* cmerge_left = thisIterSet.front(); thisIterSet.pop();
			LLVM_DEBUG(dbgs() << "CMERGE LEFT : " << cmerge_left->getIdx() << ",");
			if(thisIterSet.empty()){ //odd number of nodes

			}
			else{
				dfgNode* cmerge_right = thisIterSet.front(); thisIterSet.pop();
				//				if(thisIterSet.empty()){
				//					LLVM_DEBUG(dbgs() << "\n";
				//					continue; //no need to merge the last two;
				//				}

				LLVM_DEBUG(dbgs() << "CMERGE RIGHT : " << cmerge_right->getIdx() << ",");
				dfgNode* temp = new dfgNode(this);
				temp->setIdx(1000 + NodeList.size());
				temp->setNameType("SELECTPHI");
				temp->BB = cmerge_left->BB;
				NodeList.push_back(temp);
				selectPHIAncestorMap[temp] = selectPHIAncestorMap[cmerge_left];
				LLVM_DEBUG(dbgs() << "CREATING NEW NODE = " << temp->getIdx() << ",");

				//				assert(cmerge_left->getChildren().size() == cmerge_right->getChildren().size());
				std::set<dfgNode*> cmergeLeftChildrenOrig = mutexSetCommonChildren[cmergeSet];
				std::set<dfgNode*> cmergeRightChildrenOrig = mutexSetCommonChildren[cmergeSet];

				temp->addAncestorNode(cmerge_left); cmerge_left->addChildNode(temp);
				temp->addAncestorNode(cmerge_right); cmerge_right->addChildNode(temp);

				//				for (int i = 0; i < cmergeLeftChildrenOrig.size(); ++i) {
				for(dfgNode* child : cmergeLeftChildrenOrig){
					dfgNode* left_child = child;
					dfgNode* right_child = child;
					assert(left_child == right_child);

					cmerge_left->removeChild(left_child); left_child->removeAncestor(cmerge_left);
					cmerge_right->removeChild(right_child); right_child->removeAncestor(cmerge_right);

//					if(child->getNode()!=NULL){//remove original select instruction, and replace it with new SELECTPHI
//						if(SelectInst* SLI = dyn_cast<SelectInst>(child->getNode())){
//							for(dfgNode* selects_child : child->getChildren() ){
//								selects_child->removeAncestor(child);child->removeChild(selects_child);
//								selects_child->addAncestorNode(temp); temp->addChildNode(selects_child);
//							}
//							continue;
//						}
//					}
					left_child->addAncestorNode(temp); temp->addChildNode(left_child);
				}
				thisIterSet.push(temp);
			}
			LLVM_DEBUG(dbgs() << "\n");
		}

		LLVM_DEBUG(dbgs() << "Next ITER connections CMERGE Tree.\n");
		while(!nextIterSet.empty()){
			dfgNode* cmerge_left = nextIterSet.front(); nextIterSet.pop();
			LLVM_DEBUG(dbgs() << "CMERGE LEFT : " << cmerge_left->getIdx() << ",");
			if(nextIterSet.empty()){ //odd number of nodes

			}
			else{
				dfgNode* cmerge_right = nextIterSet.front(); nextIterSet.pop();
				//				if(nextIterSet.empty()) {
				//					LLVM_DEBUG(dbgs() << "\n";
				//					continue; //no need to merge the last two;
				//				}

				LLVM_DEBUG(dbgs() << "CMERGE RIGHT : " << cmerge_right->getIdx() << ",");
				dfgNode* temp = new dfgNode(this);
				temp->setIdx(1000 + NodeList.size());
				temp->setNameType("SELECTPHI");
				temp->BB = cmerge_left->BB;
				NodeList.push_back(temp);
				LLVM_DEBUG(dbgs() << "CREATING NEW NODE = " << temp->getIdx() << ",");
				selectPHIAncestorMap[temp] = selectPHIAncestorMap[cmerge_left];

				//				assert(cmerge_left->getChildren().size() == cmerge_right->getChildren().size());
				std::set<dfgNode*> cmergeLeftChildrenOrig = mutexSetCommonChildren[cmergeSet];
				std::set<dfgNode*> cmergeRightChildrenOrig = mutexSetCommonChildren[cmergeSet];

				temp->addAncestorNode(cmerge_left); cmerge_left->addChildNode(temp);
				temp->addAncestorNode(cmerge_right); cmerge_right->addChildNode(temp);

				//				for (int i = 0; i < cmergeLeftChildrenOrig.size(); ++i) {
				for(dfgNode* child : cmergeLeftChildrenOrig){
					dfgNode* left_child = child;
					dfgNode* right_child = child;
					assert(left_child == right_child);

					cmerge_left->removeChild(left_child); left_child->removeAncestor(cmerge_left);
					cmerge_right->removeChild(right_child); right_child->removeAncestor(cmerge_right);
					left_child->addAncestorNode(temp,EDGE_TYPE_DATA,true); temp->addChildNode(left_child,EDGE_TYPE_DATA,true);
				}
				nextIterSet.push(temp);
			}
			LLVM_DEBUG(dbgs() << "\n");
		}
	}
}

void DFGPartPred::scheduleASAP() {

	for(dfgNode* n : NodeList){
		n->setASAPnumber(-1);
	}

	std::queue<std::vector<dfgNode*>> q;
	std::vector<dfgNode*> qv;
	for(std::pair<BasicBlock*,dfgNode*> p : startNodes){
		qv.push_back(p.second);
	}

	for(dfgNode* n : NodeList){
		//		if(n->getNameType() == "OutLoopLOAD"){
		if(n->getAncestors().size() == 0){
			qv.push_back(n);
		}
		//		}
	}

	q.push(qv);

	int level = 0;

	std::set<dfgNode*> visitedNodes;

	LLVM_DEBUG(dbgs() << "Schedule ASAP ..... \n");


	while(!q.empty()){
		std::vector<dfgNode*> q_element = q.front(); q.pop();
		std::vector<dfgNode*> nqv; nqv.clear();

		std::set<dfgNode*> nqv_set; nqv_set.clear();
		std::pair<std::set<dfgNode*>::iterator,bool> ret;

		if(level > maxASAPLevel) maxASAPLevel = level;


		for(dfgNode* node : q_element){
			visitedNodes.insert(node);

			if(node->getASAPnumber() < level){
				node->setASAPnumber(level);
			}



			for(dfgNode* child : node->getChildren()){
				bool isBackEdge = node->childBackEdgeMap[child];
				if(!isBackEdge){
					//					nqv.push_back(child);
					ret = nqv_set.insert(child);
					if(ret.second == true){
						nqv.push_back(child);
					}
				}
			}

		}
		if(!nqv.empty()){
			q.push(nqv);
		}
		level++;
	}
	assert(visitedNodes.size() == NodeList.size());
}

void DFGPartPred::scheduleALAP() {

	for(dfgNode* n : NodeList){
		n->setALAPnumber(-1);
	}

	std::queue<std::vector<dfgNode*>> q;
	std::vector<dfgNode*> qv;

	assert(maxASAPLevel != 0);

	//initially fill this with childless nodes
	for(dfgNode* node : NodeList){
		int childCount = 0;
		for(dfgNode* child : node->getChildren()){
			if(node->childBackEdgeMap[child]) continue;
			childCount++;
		}
		if(childCount == 0){
			qv.push_back(node);
		}
	}
	q.push(qv);

	int level = maxASAPLevel;
	std::set<dfgNode*> visitedNodes;

	LLVM_DEBUG(dbgs() << "Schedule ALAP ..... \n");

	while(!q.empty()){
		assert(level >= 0);
		std::vector<dfgNode*> q_element = q.front(); q.pop();
		std::vector<dfgNode*> nq; nq.clear();

		std::set<dfgNode*> nq_set; nq_set.clear();
		std::pair<std::set<dfgNode*>::iterator,bool> ret;

		for(dfgNode* node : q_element){
			visitedNodes.insert(node);

			if(node->getALAPnumber() > level || node->getALAPnumber() == -1){
				node->setALAPnumber(level);
			}

			for(dfgNode* parent : node->getAncestors()){
				if(parent->childBackEdgeMap[node]) continue;

				//				nq.push_back(parent);
				ret = nq_set.insert(parent);
				if(ret.second == true){
					nq.push_back(parent);
				}
			}
		}

		if(!nq.empty()) q.push(nq);

		level--;
	}


	assert(visitedNodes.size() == NodeList.size());
}

void DFGPartPred::balanceSched() {

	for(dfgNode* node : NodeList){

		int asap = node->getASAPnumber();

		int leastASAPChild = node->getALAPnumber();
		for(dfgNode* child : node->getChildren()){
			if(node->childBackEdgeMap[child]) continue;
			if(child->getASAPnumber() < leastASAPChild){
				leastASAPChild = child->getASAPnumber();
			}
		}

		if(node->getALAPnumber() == maxASAPLevel) continue; // no need balance last level nodes

		int diff = leastASAPChild - asap;

		int new_asap = asap + diff/2;
		node->setASAPnumber(new_asap);
	}

}
