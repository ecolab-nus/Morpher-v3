#include "DfgPartPredInternal.h"

void DFGPartPred::assignALAPasASAP() {

	for(dfgNode* node : NodeList){
		node->setASAPnumber(node->getALAPnumber());
	}

}

void DFGPartPred::addRecConnsAsPseudo() {

	scheduleASAP();
	scheduleALAP();

	for(dfgNode* node : NodeList){
		for(dfgNode* recParent : node->getRecAncestors()){
			LLVM_DEBUG(dbgs() << "Adding rec Pseudo Connection :: parent=" << recParent->getIdx() << ",to" << node->getIdx() << "\n");
			removeEdge(findEdge(recParent,node));
			recParent->addChildNode(node,EDGE_TYPE_PS,false,true,true);
			node->addAncestorNode(recParent,EDGE_TYPE_PS,false,true,true);
		}
	}

	scheduleASAP();
	scheduleALAP();

}

void DFGPartPred::addOrphanPseudoEdges() {

	scheduleASAP();
	scheduleALAP();

	std::set<dfgNode*> RecParents;
	std::map<dfgNode*,int> maxDelayASAP;

	for(dfgNode* node : NodeList){
		for(dfgNode* recParent : node->getRecAncestors()){
			LLVM_DEBUG(dbgs() << "child = " << node->getIdx() << ", recparent = " << recParent->getIdx() << "\n");
			if(recParent->getALAPnumber() == recParent->getASAPnumber()) continue;
			RecParents.insert(recParent);
		}

		for(dfgNode* child : node->getChildren()){
			if(node->childBackEdgeMap[child]){
				if(child->getALAPnumber() == child->getASAPnumber()) continue;
				RecParents.insert(child);
			}
		}
	}

	for(dfgNode* n : RecParents){
		maxDelayASAP[n]=10000000;
		for(dfgNode* child : n->getChildren()){
			if(n->childBackEdgeMap[child]) continue;
			if(child->getASAPnumber() < maxDelayASAP[n]){
				maxDelayASAP[n] = child->getASAPnumber();
			}
		}
	}

	for(dfgNode* node : RecParents){
		if(node->getNode() && dyn_cast<PHINode>(node->getNode())){
			LLVM_DEBUG(dbgs() << "Skipping orphan pseudo edge for PHI node=" << node->getIdx() << "\n");
			continue;
		}

		LLVM_DEBUG(dbgs() << "Searching for recParent=" << node->getIdx() << "\n");

		//		for(dfgNode* parCand : NodeList){
		//			if(parCand->getASAPnumber() == node->getALAPnumber()-1 && parCand->getALAPnumber() == node->getALAPnumber()-1){
		//				LLVM_DEBUG(dbgs() << "Adding Pseudo Connection :: parent=" << parCand->getIdx() << ",to" << node->getIdx() << "\n";
		//				parCand->addChildNode(node,EDGE_TYPE_PS,false,true,true);
		//				node->addAncestorNode(parCand,EDGE_TYPE_PS,false,true,true);
		//				node->parentClassification[0]=parCand;
		//				break;
		//			}
		//		}

		std::queue<std::set<dfgNode*>> q;
		std::set<dfgNode*> q_init;
		for(dfgNode* child : node->getChildren()){
			if(node->childBackEdgeMap[child]) continue;
			q_init.insert(child);
		}
		q.push(q_init);

		dfgNode* criticalChild = NULL;
		int startdiff = node->getALAPnumber() - node->getASAPnumber();
		dfgNode* critCand = NULL;

		while(!q.empty()){
			std::set<dfgNode*> curr = q.front(); q.pop();
			std::set<dfgNode*> q_next;
			bool critChildFound=false;
			for(dfgNode* n : curr){
				if(n->getASAPnumber() == n->getALAPnumber()){
					criticalChild=n;
					critChildFound=true;
					startdiff=0;
					break;
				}

				int currDiff = n->getALAPnumber() - n->getASAPnumber();
				if(currDiff < startdiff){
					startdiff = currDiff;
					critCand = n;
				}

				for(dfgNode* child : n->getChildren()){
					if(n->childBackEdgeMap[child]) continue;
					q_next.insert(child);
				}
			}
			if(critChildFound) break;

			if(!q_next.empty()) q.push(q_next);
		}

		while(!q.empty()) q.pop();

		if(!criticalChild){
			if(!critCand){
				LLVM_DEBUG(dbgs() << "NO critChild and NO critCand! continue...\n");
				continue;
			}
			LLVM_DEBUG(dbgs() << "No critChild , but critCand = " << critCand->getIdx()  << ",with startDiff = " << startdiff << "\n");
			criticalChild = critCand;
			//			for(dfgNode* n : NodeList){
			//				if(n == node) continue;
			//				if(n->getALAPnumber() == node->getALAPnumber() - 1){
			//
			//					dfgNode* pseduoParent = n;
			//					LLVM_DEBUG(dbgs() << "Adding Pseudo Connection :: parent=" << pseduoParent->getIdx() << ",to" << node->getIdx() << "\n";
			//					pseduoParent->addChildNode(node,EDGE_TYPE_PS,false);
			//					node->addAncestorNode(pseduoParent,EDGE_TYPE_PS,false);
			//				}
			//			}
			//			continue;
		}

		assert(criticalChild);
		q_init.clear();
		q_init.insert(criticalChild);
		q.push(q_init);

		dfgNode* pseduoParent = NULL;
		while(!q.empty()){
			std::set<dfgNode*> curr = q.front(); q.pop();
			std::set<dfgNode*> q_next;
			bool pseudoParentFound=false;
			for(dfgNode* n : curr){
				//				if(n->getASAPnumber() == n->getALAPnumber()){
				//					if(n->getALAPnumber() == node->getALAPnumber()){
				LLVM_DEBUG(dbgs() << "n=" << n->getIdx() << ",diff=" << n->getALAPnumber() - n->getASAPnumber() << "\n");
				if(n->getALAPnumber() - n->getASAPnumber() <= startdiff &&
						n->getALAPnumber() < node->getALAPnumber() &&
						(n->getASAPnumber() < maxDelayASAP[node] - 1)
				){
					pseduoParent=n;

					std::vector<dfgNode*> node_childs = node->getChildren();

					if(std::find(node_childs.begin(),node_childs.end(),pseduoParent) == node_childs.end()
							&& !node->isParent(pseduoParent)){
						//pseudoparent is not a child of the node
						LLVM_DEBUG(dbgs() << "Adding Pseudo Connection :: parent=" << pseduoParent->getIdx() << ",to" << node->getIdx() << "\n");
						pseduoParent->addChildNode(node,EDGE_TYPE_PS,false,true,true);
						node->addAncestorNode(pseduoParent,EDGE_TYPE_PS,false,true,true);

						pseudoParentFound=true;
						break;
					}
				}
				//				}
				for(dfgNode* parent : n->getAncestors()){
					if(parent->childBackEdgeMap[n]) continue;
					if(parent == node) continue;
					q_next.insert(parent);
				}
			}
			if(pseudoParentFound) break;

			if(!q_next.empty()) q.push(q_next);
		}

		assert(pseduoParent);

		//		LLVM_DEBUG(dbgs() << "Adding Pseudo Connection :: parent=" << pseduoParent->getIdx() << ",to" << node->getIdx() << "\n";
		//		pseduoParent->addChildNode(node,EDGE_TYPE_PS,false,true,true);
		//		node->addAncestorNode(pseduoParent,EDGE_TYPE_PS,false,true,true);


		//		std::map<int,dfgNode*> asapchild;
		//		for(dfgNode* child : node->getChildren()){
		//			if(node->childBackEdgeMap[child]) continue;
		//			asapchild[child->getASAPnumber()]=child;
		//		}
		//
		//		assert(!asapchild.empty());
		//		dfgNode* earliestChild = (*asapchild.begin()).second;
		//
		//		std::map<int,dfgNode*> asapcousin;
		//		for(dfgNode* parent : earliestChild->getAncestors()){
		//			if(parent == node) continue;
		//			if(parent->childBackEdgeMap[earliestChild]) continue;
		//			asapcousin[parent->getASAPnumber()]=parent;
		//		}
		//
		//		if(!asapcousin.empty()){
		//			dfgNode* latestCousin = (*asapcousin.rbegin()).second;
		//
		//			LLVM_DEBUG(dbgs() << "Adding Pseudo Connection :: parent=" << latestCousin->getIdx() << ",to" << node->getIdx() << "\n";
		//			latestCousin->addChildNode(node,EDGE_TYPE_PS,false,true,true);
		//			node->addAncestorNode(latestCousin,EDGE_TYPE_PS,false,true,true);
		//			node->parentClassification[0]=latestCousin;
		//		}


	}
	//	assert(false);
	scheduleASAP();
	scheduleALAP();



}

dfgNode* DFGPartPred::addLoadParent(Value* ins, dfgNode* child) {
	dfgNode* temp;
	if(OutLoopNodeMap.find(ins) == OutLoopNodeMap.end()){
		temp = new dfgNode(this);
		temp->setNameType("OutLoopLOAD");
		temp->setIdx(getNodesPtr()->size());
		temp->BB = child->BB;
		getNodesPtr()->push_back(temp);
		OutLoopNodeMap[ins] = temp;
		OutLoopNodeMapReverse[temp]=ins;

		// Use a synthetic name for unnamed values (e.g., unnamed loop-bound scalars)
		// to avoid empty BasePointerName in the DFG XML and mem_alloc.txt.
		std::string baseName = ins->getName().empty()
			? ("_ol_" + std::to_string(getNodesPtr()->size() - 1))
			: ins->getName().str();
		temp->setArrBasePtr(baseName);
		DataLayout DL = temp->BB->getParent()->getParent()->getDataLayout();
		array_pointer_sizes[baseName] = DL.getTypeAllocSize(ins->getType());

		if(Instruction* real_ins = dyn_cast<Instruction>(ins)){
			if(accumulatedBBs.find(real_ins->getParent())==accumulatedBBs.end()){
				temp->setTransferedByHost(true);
			}
		}

	}
	else{
		temp = OutLoopNodeMap[ins];
	}
	if (Instruction *instruction = dyn_cast<Instruction>(ins)) {
		if (MDNode *coordinates = instruction->getMetadata(
				"morpher.systolic.stationary_weight")) {
			if (coordinates->getNumOperands() != 2)
				report_fatal_error("Malformed stationary-weight coordinates");
			auto *row = mdconst::dyn_extract<ConstantInt>(coordinates->getOperand(0));
			auto *column = mdconst::dyn_extract<ConstantInt>(coordinates->getOperand(1));
			if (!row || !column)
				report_fatal_error("Non-integer stationary-weight coordinates");
			temp->setStationaryWeightCoordinates(
				(int)row->getZExtValue(), (int)column->getZExtValue());
		}
	}

	if(GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(ins)){
		temp->setTypeSizeBytes(4);
	}
	else if(AllocaInst* ALOCA = dyn_cast<AllocaInst>(ins)){
		errs() << "ALOCA\n";
		temp->setTypeSizeBytes(4);
	}
	else if(dyn_cast<PointerType>(ins->getType())){
		temp->setTypeSizeBytes(4);
	}
	else{

		if(dyn_cast<ArrayType>(ins->getType())){
			LLVM_DEBUG(dbgs() << "A\n");
		}
		if(dyn_cast<StructType>(ins->getType())){
			LLVM_DEBUG(dbgs() << "S\n");
		}
		if(dyn_cast<PointerType>(ins->getType())){
			LLVM_DEBUG(dbgs() << "P\n");
		}

		//TODO:DAC18
		//		temp->setTypeSizeBytes((ins->getType()->getIntegerBitWidth()+7)/8);
		temp->setTypeSizeBytes((ins->getType()->getPrimitiveSizeInBits()+7)/8);
	}

	return temp;
}

void DFGPartPred::printDOT(std::string fileName) {
	std::ofstream ofs;
	ofs.open(fileName.c_str());

	//Write the initial info
	ofs << "digraph Region_18 {\n";
	ofs << "\tgraph [ nslimit = \"1000.0\",\n";
	ofs <<	"\torientation = landscape,\n";
	ofs <<	"\t\tcenter = true,\n";
	ofs <<	"\tpage = \"8.5,11\",\n";
	ofs << "\tcompound=true,\n";
	ofs <<	"\tsize = \"10,7.5\" ] ;" << std::endl;

	assert(NodeList.size() != 0);

	std::map<const BasicBlock*,std::set<dfgNode*>> BBNodeList;

	for(dfgNode* node : NodeList){
		BBNodeList[node->BB].insert(node);
	}

	int cluster_idx=0;
	for(std::pair<const BasicBlock*,std::set<dfgNode*>> BBNodeSet : BBNodeList){
		const BasicBlock* BB = BBNodeSet.first;

		//		  subgraph cluster_0 {
		//		        node [style=filled];
		//		        "Item 1" "Item 2";
		//		        label = "Container A";
		//		        color=blue;
		//		    }

		//		ofs << "subgraph cluster_" << cluster_idx << " {\n";
		//		ofs << "node [style=filled]";
		for(dfgNode* node : BBNodeSet.second){
			ofs << "\""; //BEGIN NODE NAME
			ofs << "Op_" << node->getIdx();
			ofs << "\" [ fontname = \"Helvetica\" shape = box, label = \" ";
			if(node->getNode() != NULL){
				ofs << node->getNode()->getOpcodeName();
				ofs << " " << node->getNode()->getName().str() << " ";

				if(node->hasConstantVal()){
					ofs << " C=" << "0x" << std::hex << node->getConstantVal() << std::dec;
				}

				if(node->isGEP()){
					ofs << " C=" << "0x" << std::hex << node->getGEPbaseAddr() << std::dec;
				}

			}
			else{
				ofs << node->getNameType();

				if(node->getNameType() == "OuterLoopLOAD"){
					ofs << " " << OutLoopNodeMapReverse[node]->getName().str() << " ";
				}

				if(node->isOutLoop()){
					ofs << " C=" << "0x" << node->getoutloopAddr() << std::dec;
				}

				if(node->hasConstantVal()){
					ofs << " C=" << "0x" << node->getConstantVal() << std::dec;
				}
			}

			ofs << "BB=" << node->BB->getName().str();

			if(!mutexNodes[node].empty()){
				ofs << ",mutex={";
				for(dfgNode* m : mutexNodes[node]){
					ofs << m->getIdx() << ",";
				}
				ofs << "}";
			}

			if(node->getFinalIns() != NOP){
				ofs << " HyIns=" << HyCUBEInsStrings[node->getFinalIns()];
			}
			ofs << ",\n";
			ofs << node->getIdx() << ", ASAP=" << node->getASAPnumber();
			ofs << ", ALAP=" << node->getALAPnumber();

			ofs << "\"]\n"; //END NODE NAME
		}

		//		ofs << "label = " << "\"" << BB->getName().str() << "\"" << ";\n";
		//		ofs << "color = purple;\n";
		//		ofs << "}\n";
		cluster_idx++;
	}


	//The EDGES

	for(dfgNode* node : NodeList){

		for(dfgNode* child : node->getChildren()){
			bool isCondtional=false;
			bool condition=true;

			isCondtional= node->childConditionalMap[child] != UNCOND;
			condition = (node->childConditionalMap[child] == TRUE);

			bool isBackEdge = node->childBackEdgeMap[child];


			ofs << "\"" << "Op_" << node->getIdx() << "\"";
			ofs << " -> ";
			ofs << "\"" << "Op_" << child->getIdx() << "\"";
			ofs << " [style = ";

			if(isBackEdge){
				ofs << "dashed";
			}
			else{
				ofs << "bold";
			}
			ofs << ", ";

			ofs << "color = ";
			if(isCondtional){
				if(findEdge(node,child)->getType() == EDGE_TYPE_PS){
					ofs << "cyan";
				}
				else if(condition==true){
					ofs << "blue";
				}
				else{
					ofs << "red";
				}
			}
			else{
				ofs << "black";

			}

			//			ofs << ", headport=n, tailport=s";
			ofs << "];\n";
		}

		for(dfgNode* recChild : node->getRecChildren()){
			ofs << "\"" << "Op_" << node->getIdx() << "\"";
			ofs << " -> ";
			ofs << "\"" << "Op_" << recChild->getIdx() << "\"";
			ofs << "[style = bold, color = green];\n";
		}
	}

	ofs << "}" << std::endl;
	ofs.close();
}
/*
 * DOT file with only operation name inside nodes
 * */
void DFGPartPred::printDOTsimple(std::string fileName) {
	std::ofstream ofs;
	ofs.open(fileName.c_str());

	//Write the initial info
	ofs << "digraph Region_18 {\n";
	ofs << "\tgraph [ nslimit = \"1000.0\",\n";
	ofs <<	"\torientation = landscape,\n";
	ofs <<	"\t\tcenter = true,\n";
	ofs <<	"\tpage = \"8.5,11\",\n";
	ofs << "\tcompound=true,\n";
	ofs <<	"\tsize = \"10,7.5\" ] ;" << std::endl;

	assert(NodeList.size() != 0);

	std::map<const BasicBlock*,std::set<dfgNode*>> BBNodeList;

	for(dfgNode* node : NodeList){
		BBNodeList[node->BB].insert(node);
	}

	int cluster_idx=0;
	for(std::pair<const BasicBlock*,std::set<dfgNode*>> BBNodeSet : BBNodeList){
		const BasicBlock* BB = BBNodeSet.first;

		//		  subgraph cluster_0 {
		//		        node [style=filled];
		//		        "Item 1" "Item 2";
		//		        label = "Container A";
		//		        color=blue;
		//		    }

		//		ofs << "subgraph cluster_" << cluster_idx << " {\n";
		//		ofs << "node [style=filled]";
		for(dfgNode* node : BBNodeSet.second){
			ofs << "\""; //BEGIN NODE NAME
			ofs << "Op_" << node->getIdx();
			ofs << "\" [ fontname = \"Helvetica\" shape = box, label = \" ";
//			if(node->getNode() != NULL){
//				ofs << node->getNode()->getOpcodeName();
//				ofs << " " << node->getNode()->getName().str() << " ";
//
//				if(node->hasConstantVal()){
//					ofs << " C=" << "0x" << std::hex << node->getConstantVal() << std::dec;
//				}
//
//				if(node->isGEP()){
//					ofs << " C=" << "0x" << std::hex << node->getGEPbaseAddr() << std::dec;
//				}
//
//			}
//			else{
//				ofs << node->getNameType();
//
//				if(node->getNameType() == "OuterLoopLOAD"){
//					ofs << " " << OutLoopNodeMapReverse[node]->getName().str() << " ";
//				}
//
//				if(node->isOutLoop()){
//					ofs << " C=" << "0x" << node->getoutloopAddr() << std::dec;
//				}
//
//				if(node->hasConstantVal()){
//					ofs << " C=" << "0x" << node->getConstantVal() << std::dec;
//				}
//			}

//			ofs << "BB=" << node->BB->getName().str();

//			if(!mutexNodes[node].empty()){
////				ofs << ",mutex={";
////				for(dfgNode* m : mutexNodes[node]){
////					ofs << m->getIdx() << ",";
////				}
////				ofs << "}";
//			}

			if(node->getFinalIns() != NOP){
				ofs << "" << HyCUBEInsStrings[node->getFinalIns()];
			}
//			ofs << ",\n";
//			ofs << node->getIdx() << ", ASAP=" << node->getASAPnumber();
//			ofs << ", ALAP=" << node->getALAPnumber();

			ofs << "\"]\n"; //END NODE NAME
		}

		//		ofs << "label = " << "\"" << BB->getName().str() << "\"" << ";\n";
		//		ofs << "color = purple;\n";
		//		ofs << "}\n";
		cluster_idx++;
	}


	//The EDGES

	for(dfgNode* node : NodeList){

		for(dfgNode* child : node->getChildren()){
			bool isCondtional=false;
			bool condition=true;

			isCondtional= node->childConditionalMap[child] != UNCOND;
			condition = (node->childConditionalMap[child] == TRUE);

			bool isBackEdge = node->childBackEdgeMap[child];
			if(findEdge(node,child)->getType() == EDGE_TYPE_PS){
				continue;
			}

			ofs << "\"" << "Op_" << node->getIdx() << "\"";
			ofs << " -> ";
			ofs << "\"" << "Op_" << child->getIdx() << "\"";
			ofs << " [style = ";

			if(isBackEdge){
				ofs << "dashed";
			}
			else{
				ofs << "bold";
			}
			ofs << ", ";

			ofs << "color = ";
			if(isCondtional){
				if(findEdge(node,child)->getType() == EDGE_TYPE_PS){
					ofs << "black";
				}
				else if(condition==true){
					ofs << "black";
				}
				else{
					ofs << "black";
				}
			}
			else{
				ofs << "black";

			}

			//			ofs << ", headport=n, tailport=s";
			ofs << "];\n";
		}

//		for(dfgNode* recChild : node->getRecChildren()){
//			ofs << "\"" << "Op_" << node->getIdx() << "\"";
//			ofs << " -> ";
//			ofs << "\"" << "Op_" << recChild->getIdx() << "\"";
//			ofs << "[style = bold, color = black];\n";
//		}
	}

	ofs << "}" << std::endl;
	ofs.close();
}
