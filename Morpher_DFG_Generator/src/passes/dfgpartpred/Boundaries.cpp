#include "DfgPartPredInternal.h"

void DFGPartPred::printNewDFGXML() {


	std::string fileName = kernelname + "_PartPredDFG.xml";
	std::ofstream xmlFile;
	xmlFile.open(fileName.c_str());



	//    insertMOVC();
	//	scheduleASAP();
	//	scheduleALAP();
	//	balanceASAPALAP();
	//				  LoopDFG.addBreakLongerPaths();
	CreateSchList();

	std::map<BasicBlock*,std::set<BasicBlock*>> mBBs = checkMutexBBs();
	std::map<std::string,std::set<std::string>> mBBs_str;

	std::map<int,std::vector<dfgNode*>> asaplevelNodeList;
	for (dfgNode* node : NodeList){
		asaplevelNodeList[node->getASAPnumber()].push_back(node);
	}

	std::map<dfgNode*,std::string> nodeBBModified;
	for(dfgNode* node : NodeList){
		nodeBBModified[node]=node->BB->getName().str();
	}


	for(dfgNode* node : NodeList){
		int cmergeParentCount=0;
		std::set<std::string> mutexBBs;
		for(dfgNode* parent: node->getAncestors()){
			if(HyCUBEInsStrings[parent->getFinalIns()] == "CMERGE" || parent->getNameType() == "SELECTPHI"){
				nodeBBModified[parent]=nodeBBModified[parent]+ "_" + std::to_string(node->getIdx()) + "_" + std::to_string(cmergeParentCount);
				mutexBBs.insert(nodeBBModified[parent]);
				cmergeParentCount++;
			}
		}
		for(std::string bb_str1 : mutexBBs){
			for(std::string bb_str2 : mutexBBs){
				if(bb_str2==bb_str1) continue;
				mBBs_str[bb_str1].insert(bb_str2);
			}
		}
	}


	xmlFile << "<MutexBB>\n";
	for(std::pair<BasicBlock*,std::set<BasicBlock*>> pair : mBBs){
		BasicBlock* first = pair.first;
		xmlFile << "<BB1 name=\"" << first->getName().str() << "\">\n";
		for(BasicBlock* second : pair.second){
			xmlFile << "\t<BB2 name=\"" << second->getName().str() << "\"/>\n";
		}
		xmlFile << "</BB1>\n";
	}
	for(std::pair<std::string,std::set<std::string>> pair : mBBs_str){
		std::string first = pair.first;
		xmlFile << "<BB1 name=\"" << first << "\">\n";
		for(std::string second : pair.second){
			xmlFile << "\t<BB2 name=\"" << second << "\"/>\n";
		}
		xmlFile << "</BB1>\n";
	}
	xmlFile << "</MutexBB>\n";

	xmlFile << "<DFG count=\"" << NodeList.size() << "\">\n";
	std::cout << "DFG node count: " << NodeList.size() << "\n";

	for(dfgNode* node : NodeList){
		//	for (int i = 0; i < maxASAPLevel; ++i) {
		//		for(dfgNode* node : asaplevelNodeList[i]){
		xmlFile << "<Node idx=\"" << node->getIdx() << "\"";
		xmlFile << " ASAP=\"" << node->getASAPnumber() << "\"";
		xmlFile << " ALAP=\"" << node->getALAPnumber() << "\"";

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
		if(node->hasConstantVal()){
			xmlFile << " CONST=\"" << node->getConstantVal() << "\"";
		}
		if(node->getSimdGroup() >= 0 && node->getSimdLanes() > 1){
			xmlFile << " simd_group=\"" << node->getSimdGroup() << "\"";
			xmlFile << " simd_lanes=\"" << node->getSimdLanes() << "\"";
		}
		if(!node->getSimdReduceOp().empty()){
			xmlFile << " simd_reduce_op=\"" << node->getSimdReduceOp() << "\"";
		}
		if(!node->getSimdReduceBase().empty()){
			xmlFile << " simd_reduce_base=\"" << node->getSimdReduceBase() << "\"";
		}
		if(!node->getSimdAccumulatorOp().empty()){
			xmlFile << " simd_accumulator_op=\"" << node->getSimdAccumulatorOp() << "\"";
		}
		if(!node->getSimdAccumulatorInitVar().empty()){
			xmlFile << " simd_accumulator_init_var=\"" << node->getSimdAccumulatorInitVar() << "\"";
		}
		if(!node->getSimdAccumulatorExitVar().empty()){
			xmlFile << " simd_accumulator_exit_var=\"" << node->getSimdAccumulatorExitVar() << "\"";
		}
		if(!node->getSimdAccumulatorSeedVar().empty()){
			xmlFile << " simd_accumulator_seed_var=\"" << node->getSimdAccumulatorSeedVar() << "\"";
		}
		if(node->getSimdPacked()){
			xmlFile << " simd_packed=\"true\"";
			xmlFile << " simd_lane_sources=\"";
			const std::vector<int>& laneSources = node->getSimdLaneSources();
			for(size_t lane = 0; lane < laneSources.size(); ++lane){
				if(lane != 0) xmlFile << ",";
				xmlFile << laneSources[lane];
			}
			xmlFile << "\"";
		}
		if(node->isLoopCarriedState()){
			xmlFile << " loop_carried_state=\"true\"";
			xmlFile << " state_storage=\""
				<< (node->isStatefulTreg() ? "treg" : "explicit_recurrence")
				<< "\"";
		}
		if(!node->getSystolicMemStream().empty()){
			xmlFile << " systolic_mem_stream=\""
				<< node->getSystolicMemStream() << "\"";
			if(node->getSystolicMemStreamIndex() >= 0)
				xmlFile << " systolic_mem_stream_index=\""
					<< node->getSystolicMemStreamIndex() << "\"";
		}
		if(!node->getSystolicDataflow().empty()){
			xmlFile << " systolic_dataflow=\""
				<< node->getSystolicDataflow() << "\"";
		}
		if(node->getSystolicMacRow() >= 0 && node->getSystolicMacColumn() >= 0){
			xmlFile << " systolic_mac_row=\"" << node->getSystolicMacRow()
				<< "\" systolic_mac_column=\""
				<< node->getSystolicMacColumn() << "\"";
		}
		if(node->hasStationaryWeightOperand()){
			xmlFile << " stationary_weight_operand=\"I"
				<< node->getStationaryWeightOperand() << "\"";
			xmlFile << " stationary_weight_trace_var=\""
				<< node->getStationaryWeightTraceVar() << "\"";
			xmlFile << " stationary_weight_byte_offset=\""
				<< node->getStationaryWeightByteOffset() << "\"";
		}
		xmlFile << ">\n";

		std::string opName = HyCUBEInsStrings[node->getFinalIns()];
		if((node->getNameType() == "OutLoopLOAD") || (node->getNameType() == "OutLoopSTORE") ){
			if(opName.size() > 1 && opName[0] == 'V')
				opName = "VO" + opName.substr(1);
			else
				opName = "O" + opName;
		}
		xmlFile << "<OP>" << opName << "</OP>\n";

		if(node->getArrBasePtr() != "NOT_A_MEM_OP"){
			xmlFile << "<BasePointerName size=\"" << array_pointer_sizes[node->getArrBasePtr()] << "\">";
			xmlFile << node->getArrBasePtr();
			xmlFile << "</BasePointerName>\n";
		}

		if(node->getNameType() == "LOOPSTART")
			xmlFile << "<BasePointerName size=\"1\">loopstart</BasePointerName>\n";
		if(node->getNameType() == "LOOPEXIT")
			xmlFile << "<BasePointerName size=\"1\">loopend</BasePointerName>\n";
		if(node->getNameType() == "STORESTART")
			xmlFile << "<BasePointerName size=\"1\">loopstart</BasePointerName>\n";
		if(node->getGEPbaseAddr() != -1){
			GetElementPtrInst* GEP = cast<GetElementPtrInst>(node->getNode());
			int gep_offset = GEPOffsetMap[GEP];
			xmlFile << "<GEPOffset>";
			xmlFile << gep_offset;
			xmlFile << "</GEPOffset>\n";
		}

		//			xmlFile << "<OP>" << HyCUBEInsStrings[node->getFinalIns()] << "</OP>\n";

		xmlFile << "<Inputs>\n";
		for(dfgNode* parent : node->getAncestors()){
			//			xmlFile << "\t<Input idx=\"" << parent->getIdx() << "\" type=\"DATA\"/>\n";
			xmlFile << "\t<Input idx=\"" << parent->getIdx() << "\"/>\n";
		}
		//		for(dfgNode* parentPHI : node->getPHIancestors()){
		//			xmlFile << "\t<Input idx=\"" << parentPHI->getIdx() << "\" type=\"PHI\"/>\n";
		//		}
		xmlFile << "</Inputs>\n";

		xmlFile << "<Outputs>\n";
		for(dfgNode* child : node->getChildren()){
			//			xmlFile << "\t<Output idx=\"" << child->getIdx() <<"\" type=\"DATA\"/>\n";
			xmlFile << "\t<Output idx=\"" << child->getIdx() << "\" ";

			if(node->childBackEdgeMap[child]){
				xmlFile << "nextiter=\"1\" ";
			}
			else{
				xmlFile << "nextiter=\"0\" ";
			}


			bool written=false;
			if(findEdge(node,child)->getType() == EDGE_TYPE_PS){
				xmlFile << "type=\"PS\"/>\n";
				written=true;
			}
			else if(node->getNameType()=="CMERGE"){
				if(child->getNode()){
					if(dyn_cast<PHINode>(child->getNode())){

					}
					else if(dyn_cast<SelectInst>(child->getNode())){

					}
					else{ // if not phi
						written = true;
						int operand_no = findOperandNumber(child,child->getNode(),cmergePHINodes[node]->getNode());
						if( operand_no == 0){
							child->parentClassification[0]=node;
							if(child->getNPB()){
								xmlFile << "NPB=\"1\" ";
							}
							else{
								xmlFile << "NPB=\"0\" ";
							}
							xmlFile << "type=\"P\"/>\n";
						}
						else if ( operand_no == 1){
							child->parentClassification[1]=node;
							xmlFile << "type=\"I1\"/>\n";
						}
						else if(( operand_no == 2)){
							child->parentClassification[2]=node;
							xmlFile << "type=\"I2\"/>\n";
						}
						else{
							LLVM_DEBUG(dbgs() << "node=" << node->getIdx() << ",child=" << child->getIdx() << "\n");
							DFG_CHECK(false, "generateXML",
								"unexpected operand number for node " + std::to_string(node->getIdx()) +
								" -> child " + std::to_string(child->getIdx()) + " (unsupported instruction pattern)");
						}
					}
				}
			}
			else if(node->getNameType()=="SELECTPHI" && (child != selectPHIAncestorMap[node])){
				if(child->getNode()){
					written = true;
					LLVM_DEBUG(dbgs() << "SELECTPHI :: " << node->getIdx());
					LLVM_DEBUG(dbgs() << ",child = " << child->getIdx() << " | "); LLVM_DEBUG(child->getNode()->dump());
					LLVM_DEBUG(dbgs() << ",phiancestor = " << selectPHIAncestorMap[node]->getIdx() << " | ");
					LLVM_DEBUG(selectPHIAncestorMap[node]->getNode()->dump());

					int operand_no = findOperandNumber(child,child->getNode(),selectPHIAncestorMap[node]->getNode());
					if( operand_no == 0){
						child->parentClassification[0]=node;
						if(child->getNPB()){
							xmlFile << "NPB=\"1\" ";
						}
						else{
							xmlFile << "NPB=\"0\" ";
						}
						xmlFile << "type=\"P\"/>\n";
					}
					else if ( operand_no == 1){
						child->parentClassification[1]=node;
						xmlFile << "type=\"I1\"/>\n";
					}
					else if(( operand_no == 2)){
						child->parentClassification[2]=node;
						xmlFile << "type=\"I2\"/>\n";
					}
					else{
						LLVM_DEBUG(dbgs() << "node = " << node->getIdx() << ", child = " << child->getIdx() << "\n");
						DFG_CHECK(false, "generateXML",
							"unexpected operand number for SELECTPHI node " + std::to_string(node->getIdx()) +
							" -> child " + std::to_string(child->getIdx()) + " (unsupported SELECT pattern)");
					}
				}
			}

			if(Edge2OperandIdxMap.find(node) != Edge2OperandIdxMap.end()
					&& Edge2OperandIdxMap[node].find(child) != Edge2OperandIdxMap[node].end()){
				int operand_no = Edge2OperandIdxMap[node][child];
				if( operand_no == 0){
					child->parentClassification[0]=node;
					if(child->getNPB()){
						xmlFile << "NPB=\"1\" ";
					}
					else{
						xmlFile << "NPB=\"0\" ";
					}
					xmlFile << "type=\"P\"/>\n";
				}
				else if ( operand_no == 1){
					child->parentClassification[1]=node;
					xmlFile << "type=\"I1\"/>\n";
				}
				else if(( operand_no == 2)){
					child->parentClassification[2]=node;
					xmlFile << "type=\"I2\"/>\n";
				}
				written = true;
			}

			if(written){
			}
			else if(child->parentClassification[0]==node){
				if(child->getNPB()){
					xmlFile << "NPB=\"1\" ";
				}
				else{
					xmlFile << "NPB=\"0\" ";
				}
				xmlFile << "type=\"P\"/>\n";
			}
			else if(child->parentClassification[1]==node){
				xmlFile << "type=\"I1\"/>\n";
			}
			else if(child->parentClassification[2]==node){
				xmlFile << "type=\"I2\"/>\n";
			}
			else if(child->parentClassification[3]==node){
				xmlFile << "type=\"I3\"/>\n";// type to handle single source nodes in mapper
			}
			else{
				bool found=false;
				for(std::pair<int,dfgNode*> pair : child->parentClassification){
					if(pair.second == node){
						xmlFile << "type=\"I2\"/>\n";
						found=true;
						break;
					}
				}

				if(!found){
					LLVM_DEBUG(dbgs() << "node = " << node->getIdx() <<"child getNameType= " << child->getNameType() << ", child = " << child->getIdx() << "\n");
				}

				assert(found);
			}
		}
		//		for(dfgNode* phiChild : node->getPHIchildren()){
		//			xmlFile << "\t<Output idx=\"" << phiChild->getIdx() << "\" type=\"PHI\"/>\n";
		//		}
		xmlFile << "</Outputs>\n";

		xmlFile << "<RecParents>\n";
		for(dfgNode* recParent : node->getRecAncestors()){
			xmlFile << "\t<RecParent idx=\"" << recParent->getIdx() << "\"/>\n";
		}
		xmlFile << "</RecParents>\n";

		xmlFile << "</Node>\n\n";

	}
	//		}
	//	}




	xmlFile << "</DFG>\n";





	xmlFile.close();




}

int DFGPartPred::canonicalizeScalarAccumulatorBoundaries(
	bool nativeMAC, bool tregResetOnLaunch)
{
	struct Candidate
	{
		dfgNode *select = NULL;
		dfgNode *initMerge = NULL;
		dfgNode *backMerge = NULL;
		dfgNode *update = NULL;
		dfgNode *term = NULL;
		dfgNode *product = NULL;
		dfgNode *exitTransfer = NULL;
		uint64_t seed = 0;
		int valueBits = 32;
		bool fuseMAC = false;
	};

	std::vector<Candidate> candidates;
	if (boundaryOriginalIndex.empty())
	{
		boundaryOriginalNodeCount = (int)NodeList.size();
		for (dfgNode *node : NodeList)
			boundaryOriginalIndex[node] = node->getIdx();
	}
	for (dfgNode *select : NodeList)
	{
		PHINode *phi = select->getNode()
			? dyn_cast<PHINode>(select->getNode()) : NULL;
		if (!phi || select->getFinalIns() != SELECT || !currLoop ||
			phi->getParent() != currLoop->getHeader() ||
			phi->getNumIncomingValues() != 2 ||
			!phi->getType()->isIntegerTy() ||
			phi->getType()->getIntegerBitWidth() != 32 ||
			select->getChildren().size() != 1 ||
			select->getAncestors().size() != 2)
			continue;

		Value *seedValue = NULL;
		Value *updateValue = NULL;
		for (unsigned incoming = 0; incoming < phi->getNumIncomingValues(); ++incoming)
		{
			if (currLoop->contains(phi->getIncomingBlock(incoming)))
				updateValue = phi->getIncomingValue(incoming);
			else
				seedValue = phi->getIncomingValue(incoming);
		}
		ConstantInt *seedConstant = dyn_cast_or_null<ConstantInt>(seedValue);
		Instruction *updateInstruction = dyn_cast_or_null<Instruction>(updateValue);
		if (!seedConstant || !updateInstruction)
			continue;
		BinaryOperator *updateBinary = dyn_cast<BinaryOperator>(updateInstruction);
		if (!updateBinary || updateBinary->getOpcode() != Instruction::Add ||
			(updateBinary->getOperand(0) != phi && updateBinary->getOperand(1) != phi))
			continue;

		dfgNode *update = findNode(updateInstruction);
		if (!update || select->getChildren().front() != update ||
			update->getFinalIns() != ADD)
			continue;

		dfgNode *initMerge = NULL;
		dfgNode *backMerge = NULL;
		bool valid = true;
		for (dfgNode *merge : select->getAncestors())
		{
			if (merge->getFinalIns() != CMERGE ||
				merge->getChildren().size() != 1 ||
				merge->getChildren().front() != select ||
				cmergePHINodes[merge] != select)
			{
				valid = false;
				break;
			}
			auto dataIt = cmergeDataInputs.find(merge);
			if (dataIt != cmergeDataInputs.end() && dataIt->second == update &&
				merge->childBackEdgeMap[select])
				backMerge = merge;
			else if (merge->hasConstantVal() &&
				(uint64_t)(uint32_t)merge->getConstantVal() == seedConstant->getZExtValue())
				initMerge = merge;
			else
				valid = false;
		}
		if (!valid || !initMerge || !backMerge ||
			cmergeCtrlInputs.find(initMerge) == cmergeCtrlInputs.end() ||
			cmergeCtrlInputs[initMerge]->getNameType() != "LOOPSTART")
			continue;

		dfgNode *exitTransfer = NULL;
		for (dfgNode *child : update->getChildren())
		{
			if (child == backMerge)
				continue;
			if (exitTransfer || child->getNameType() != "OutLoopSTORE" ||
				child->getAncestors().size() != 1 || !child->getChildren().empty())
			{
				valid = false;
				break;
			}
			exitTransfer = child;
		}
		if (!valid || !exitTransfer || update->getChildren().size() != 2 ||
			exitTransfer->getArrBasePtr().empty() ||
			exitTransfer->getArrBasePtr() == "NOT_A_MEM_OP")
			continue;

		dfgNode *term = NULL;
		for (dfgNode *parent : update->getAncestors())
		{
			if (parent == select)
				continue;
			if (term)
			{
				valid = false;
				break;
			}
			term = parent;
		}
		if (!valid || !term || update->getAncestors().size() != 2)
			continue;

		Candidate candidate;
		candidate.select = select;
		candidate.initMerge = initMerge;
		candidate.backMerge = backMerge;
		candidate.update = update;
		candidate.term = term;
		candidate.exitTransfer = exitTransfer;
		candidate.seed = seedConstant->getZExtValue();
		candidate.valueBits = phi->getType()->getIntegerBitWidth();
		candidate.product = term;
		candidate.fuseMAC = nativeMAC && tregResetOnLaunch && candidate.seed == 0 &&
			term->getFinalIns() == MUL && term->getChildren().size() == 1 &&
			term->getChildren().front() == update &&
			term->getAncestors().size() == 2 &&
			term->getAncestors()[0] != term->getAncestors()[1];
		candidates.push_back(candidate);
	}

	std::unordered_set<dfgNode*> removed;
	for (Candidate& candidate : candidates)
	{
		candidate.update->setLoopCarriedState(true);
		BoundaryState boundary;
		boundary.mappedNode = candidate.update;
		boundary.valueBits = candidate.valueBits;
		boundary.seedValue = candidate.seed;
		boundary.finalTraceVars.push_back(candidate.exitTransfer->getArrBasePtr());
		boundaryPrunedArtifactVars.insert(candidate.exitTransfer->getArrBasePtr());

		candidate.select->removeChild(candidate.update);
		candidate.update->removeAncestor(candidate.select);
		if (candidate.fuseMAC)
		{
			candidate.product->removeChild(candidate.update);
			candidate.update->removeAncestor(candidate.product);
			const std::vector<dfgNode*> productParents = candidate.product->getAncestors();
			for (size_t operand = 0; operand < productParents.size(); ++operand)
			{
				dfgNode *parent = productParents[operand];
				parent->addChildNode(candidate.update);
				candidate.update->addAncestorNode(parent);
				Edge2OperandIdxMap[parent][candidate.update] = (int)operand + 1;
			}
			candidate.update->setFinalIns(MAC);
			candidate.update->setStatefulTreg(true);
			boundary.storage = "treg";
			boundary.seedKind = "register_reset";
			removed.insert(candidate.product);
		}
		else
		{
			candidate.update->addChildNode(
				candidate.update, EDGE_TYPE_DATA, true, false, true);
			candidate.update->addAncestorNode(
				candidate.update, EDGE_TYPE_DATA, true, false, true);
			Edge2OperandIdxMap[candidate.update][candidate.update] = 1;
			Edge2OperandIdxMap[candidate.term][candidate.update] = 2;
			boundary.storage = "explicit_recurrence";
			boundary.seedKind = "constant";
		}
		boundaryStates.push_back(std::move(boundary));
		removed.insert(candidate.select);
		removed.insert(candidate.initMerge);
		removed.insert(candidate.backMerge);
		removed.insert(candidate.exitTransfer);
	}

	if (removed.empty())
		return 0;

	for (dfgNode *node : NodeList)
	{
		if (removed.count(node))
			continue;
		const std::vector<dfgNode*> parents = node->getAncestors();
		for (dfgNode *parent : parents)
			if (removed.count(parent))
			{
				parent->removeChild(node);
				node->removeAncestor(parent);
			}
		const std::vector<dfgNode*> children = node->getChildren();
		for (dfgNode *child : children)
			if (removed.count(child))
			{
				node->removeChild(child);
				child->removeAncestor(node);
			}
		node->parentClassification.clear();
		node->childClassification.clear();
	}

	edgeList.erase(std::remove_if(edgeList.begin(), edgeList.end(),
		[&removed](Edge& edge) {
			return removed.count(edge.getSrc()) || removed.count(edge.getDest());
		}), edgeList.end());
	NodeList.erase(std::remove_if(NodeList.begin(), NodeList.end(),
		[&removed](dfgNode *node) { return removed.count(node); }), NodeList.end());
	for (auto it = OutLoopNodeMap.begin(); it != OutLoopNodeMap.end(); )
		if (removed.count(it->second)) it = OutLoopNodeMap.erase(it); else ++it;
	for (auto it = OutLoopNodeMapReverse.begin(); it != OutLoopNodeMapReverse.end(); )
		if (removed.count(it->first)) it = OutLoopNodeMapReverse.erase(it); else ++it;
	for (auto it = cmergeCtrlInputs.begin(); it != cmergeCtrlInputs.end(); )
		if (removed.count(it->first)) it = cmergeCtrlInputs.erase(it); else ++it;
	for (auto it = cmergeDataInputs.begin(); it != cmergeDataInputs.end(); )
		if (removed.count(it->first)) it = cmergeDataInputs.erase(it); else ++it;
	for (auto it = cmergePHINodes.begin(); it != cmergePHINodes.end(); )
		if (removed.count(it->first)) it = cmergePHINodes.erase(it); else ++it;
	for (size_t index = 0; index < NodeList.size(); ++index)
		NodeList[index]->setIdx((int)index);

	llvm::outs() << "[boundary] Canonicalized " << candidates.size()
		<< " scalar accumulator(s), fused "
		<< std::count_if(candidates.begin(), candidates.end(),
			[](const Candidate& candidate) { return candidate.fuseMAC; })
		<< " native MAC(s); remaining nodes=" << NodeList.size() << "\n";
	return (int)candidates.size();
}

int DFGPartPred::externalizeStationaryWeights()
{
	if (boundaryOriginalIndex.empty())
	{
		boundaryOriginalNodeCount = (int)NodeList.size();
		for (dfgNode *node : NodeList)
			boundaryOriginalIndex[node] = node->getIdx();
	}

	std::vector<dfgNode *> weights;
	for (dfgNode *node : NodeList)
		if (node->getNameType() == "OutLoopLOAD" &&
			node->getStationaryWeightRow() >= 0 &&
			node->getStationaryWeightColumn() >= 0)
			weights.push_back(node);
	if (weights.empty())
		return 0;

	for (dfgNode *weight : weights)
	{
		if (weight->getChildren().size() != 1)
			report_fatal_error(
				"A stationary weight must feed exactly one spatial compute node");
		dfgNode *consumer = weight->getChildren().front();
		if (consumer->getFinalIns() != MUL ||
			consumer->getSystolicDataflow() != "weight_stationary" ||
			consumer->getSystolicMacRow() != weight->getStationaryWeightRow() ||
			consumer->getSystolicMacColumn() != weight->getStationaryWeightColumn())
			report_fatal_error(
				"Stationary weight and weight-owning multiply coordinates disagree");

		int operand = -1;
		for (const auto &classified : consumer->parentClassification)
			if (classified.second == weight)
			{
				if (classified.first != 1 && classified.first != 2)
					report_fatal_error(
						"Stationary weight is not a data operand of its multiply");
				operand = classified.first;
				break;
			}
		if (operand < 0)
			report_fatal_error(
				"Stationary weight operand classification is missing");

		StationaryPreload preload;
		preload.mappedNode = consumer;
		preload.operand = operand;
		preload.valueBits = weight->getTypeSizeBytes() * 8;
		preload.row = weight->getStationaryWeightRow();
		preload.column = weight->getStationaryWeightColumn();
		preload.traceVar = weight->getArrBasePtr();
		if (preload.traceVar.empty() || preload.traceVar == "NOT_A_MEM_OP")
			report_fatal_error("Stationary weight has no trace-image variable");
		stationaryPreloads.push_back(preload);
		consumer->setStationaryWeightOperand(
			operand, preload.traceVar, preload.byteOffset);

		consumer->parentClassification.erase(operand);
		for (auto it = weight->childClassification.begin();
			it != weight->childClassification.end(); )
			if (it->second == consumer) it = weight->childClassification.erase(it);
			else ++it;
		weight->removeChild(consumer);
		consumer->removeAncestor(weight);
		if (Edge *edge = findEdge(weight, consumer))
			removeEdge(edge);
		Edge2OperandIdxMap[weight].erase(consumer);
	}

	std::unordered_set<dfgNode *> removed(weights.begin(), weights.end());
	NodeList.erase(std::remove_if(NodeList.begin(), NodeList.end(),
		[&removed](dfgNode *node) { return removed.count(node); }), NodeList.end());
	for (auto it = OutLoopNodeMap.begin(); it != OutLoopNodeMap.end(); )
		if (removed.count(it->second)) it = OutLoopNodeMap.erase(it); else ++it;
	for (auto it = OutLoopNodeMapReverse.begin(); it != OutLoopNodeMapReverse.end(); )
		if (removed.count(it->first)) it = OutLoopNodeMapReverse.erase(it); else ++it;
	for (size_t index = 0; index < NodeList.size(); ++index)
		NodeList[index]->setIdx((int)index);

	llvm::outs() << "[boundary] Externalized " << stationaryPreloads.size()
		<< " stationary weight operand(s) into launch preloads\n";
	return (int)stationaryPreloads.size();
}

int DFGPartPred::classifyParents() {

	dfgNode* node;
	int parent_count=0;
	for (int i = 0; i < NodeList.size(); ++i) {
		node = NodeList[i];

		LLVM_DEBUG(dbgs() << "classifyParent::currentNode = " << node->getIdx() << "\n");
		LLVM_DEBUG(dbgs() << "Parents : ");
		parent_count=0;
		for (dfgNode* parent : node->getAncestors()) {
			LLVM_DEBUG(dbgs() << parent->getIdx() << ",");
			parent_count++;
		}
		LLVM_DEBUG(dbgs() << "\n");

		for (dfgNode* parent : node->getAncestors()) {
			Instruction* ins;
			Value* parentIns;

			if (parent == node &&
				(!node->getSimdAccumulatorOp().empty() || node->isLoopCarriedState())) {
				node->parentClassification[1] = parent;
				continue;
			}


			if(node->getNode()!=NULL){
				ins=node->getNode();
			}
			else{
				if(node->getNameType().compare("VRED")==0){
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1]=parent;
					continue;
				}
				if(node->getNameType().compare("XORNOT")==0){
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1]=parent;
					continue;
				}
				if(node->getNameType().compare("ORZERO")==0){
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1]=parent;
					continue;
				}
					if(node->getNameType().compare("GEPLEFTSHIFT")==0 ||
					   node->getNameType().compare("GEPSCALE")==0){
						if(parent->getNode()!=NULL){
							parentIns = parent->getNode();
							if(BranchInst* BRI = dyn_cast<BranchInst>(parentIns)){
							node->parentClassification[0]=parent;
							continue;
						}
						else if(CmpInst * CMPI = dyn_cast<CmpInst>(parentIns)){
							node->parentClassification[0]=parent;
							continue;
						}
					}
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1]=parent;
					continue;
				}
				if(node->getNameType().compare("MASKAND")==0){
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1]=parent;
					continue;
				}
				if(node->getNameType().compare("PREDAND")==0){
					assert(node->parentClassification.find(1) == node->parentClassification.end());
					node->parentClassification[1]=parent;
					continue;
				}
				if(node->getNameType().compare("CTRLBrOR")==0){
					if(node->parentClassification.find(1) == node->parentClassification.end()){
						node->parentClassification[1]=parent;
					}
					else{
						assert(node->parentClassification.find(2) == node->parentClassification.end());
						node->parentClassification[2]=parent;
					}
					continue;
				}
				if(node->getNameType().compare("SELECTPHI")==0 || (realphi_as_selectphi.find(node) != realphi_as_selectphi.end())){
					if(node->parentClassification.find(1) == node->parentClassification.end()){
						node->parentClassification[1]=parent;
					}
					else{
						assert(node->parentClassification.find(2) == node->parentClassification.end());
						node->parentClassification[2]=parent;
					}
					continue;
				}
				if(node->getNameType().compare("GEPADD")==0){
					if(node->parentClassification.find(1) == node->parentClassification.end()){
						node->parentClassification[1]=parent;
					}
					else{
						assert(node->parentClassification.find(2) == node->parentClassification.end());
						node->parentClassification[2]=parent;
					}
					continue;
				}

				if(node->getNameType().compare("STORESTART")==0){
					assert(parent->getNode()==NULL);
					if(parent->getNameType().compare("MOVC") == 0){
						node->parentClassification[1]=parent;
					}
					else if(parent->getNameType().compare("LOOPSTART") == 0){
						node->parentClassification[0]=parent;
					}
					else if(parent->getNameType().compare("PREDAND") == 0){
						node->parentClassification[0]=parent;
					}
					else{
						DFG_CHECK(false, "classifyParents",
							"STORESTART node " + std::to_string(node->getIdx()) +
							" has unrecognized parent type: " + parent->getNameType());
					}
					continue;
				}
				if(node->getNameType().compare("LOOPEXIT")==0){
					if(parent->getNode()==NULL){
						if(parent->getNameType().compare("MOVC") == 0){
							//addLoopExitStoreHyCUBE might already set the classification
							//assert(node->parentClassification.find(1) == node->parentClassification.end());
							node->parentClassification[1]=parent;
						}
						else if(parent->getNameType().compare("CTRLBrOR") == 0){
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0]=parent;
						}
						else if(parent->getNameType().compare("LOOPSTART") == 0){
							assert(node->parentClassification.find(0) == node->parentClassification.end());
							node->parentClassification[0]=parent;
						}
						else{
							DFG_CHECK(false, "classifyParents",
								"LOOPEXIT node " + std::to_string(node->getIdx()) +
								" has unrecognized parent type: " + parent->getNameType());
						}
					}
					else{
						//							assert(node->parentClassification.find(0) == node->parentClassification.end());
						node->parentClassification[0]=parent;
					}
					continue;
				}
				if(node->getNameType().compare("OutLoopLOAD")==0){
					//do nothing
					assert(node->parentClassification.find(0) == node->parentClassification.end());
					node->parentClassification[0]=parent;
					continue;
				}
				if(node->getNameType().compare("OutLoopSTORE")==0){
					assert(node->hasConstantVal());

					//check for predicate parent
					if(parent->getNode()!=NULL){
						parentIns = parent->getNode();
						if(BranchInst* BRI = dyn_cast<BranchInst>(parentIns)){
							node->parentClassification[0]=parent;
							continue;
						}
						else if(CmpInst * CMPI = dyn_cast<CmpInst>(parentIns)){
							node->parentClassification[0]=parent;
							continue;
						}
					}
					else{
						if(parent->getNameType().compare("CTRLBrOR")==0){
							node->parentClassification[0]=parent;
							continue;
						}
					}

					//if it comes to this, it has to be a data parent.
					node->parentClassification[1]=parent;
					continue;
				}
				if(node->getNameType().compare("CMERGE")==0){

					if(cmergeCtrlInputs[node] == parent){
						node->parentClassification[0] = parent;
					}
					else if(cmergeDataInputs[node] == parent){
						node->parentClassification[1] = parent;
					}
					else{
						DFG_CHECK(false, "classifyParents",
							"CMERGE node " + std::to_string(node->getIdx()) +
							" has parent " + std::to_string(parent->getIdx()) +
							" that is neither ctrl nor data input");
					}
					continue;

				}
			}


			if(dyn_cast<PHINode>(ins)){
				if(parent->getNode() != NULL) LLVM_DEBUG(parent->getNode()->dump());
				LLVM_DEBUG(dbgs() << "node : " << node->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "Parent : " << parent->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "Parent NameType : " << parent->getNameType() << "\n");

				if(parent->getNode()){
					if(dyn_cast<BranchInst>(parent->getNode())){
						node->parentClassification[0]=parent;
						continue;
					}
				}

				assert(parent->getNameType().compare("CMERGE")==0 || parent->getNameType().compare("SELECTPHI")==0);
				if(node->parentClassification.find(1) == node->parentClassification.end()){
					node->parentClassification[1]=parent;
				}
				else if(node->parentClassification.find(2) == node->parentClassification.end()){
					//						assert(node->parentClassification.find(2) == node->parentClassification.end());
					node->parentClassification[2]=parent;
				}
				else{
					LLVM_DEBUG(dbgs() << "Extra parent : " << parent->getIdx() << ",Nametype = " << parent->getNameType() << ",par_idx = " << node->parentClassification.size()+1 << "\n");
					node->parentClassification[node->parentClassification.size()+1]=parent;
				}
				continue;
			}

			if( dyn_cast<SelectInst>(ins) ){

				LLVM_DEBUG(dbgs() << "node : " << node->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "Parent : " << parent->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "Parent NameType : " << parent->getNameType() << "\n");

				if(parent->getNode()){
					if(dyn_cast<BranchInst>(parent->getNode())){
						node->parentClassification[0]=parent;
						continue;
					}
				}

				assert(parent->getNameType().compare("CMERGE")==0 || parent->getNameType().compare("SELECTPHI")==0);
				if(node->parentClassification.find(1) == node->parentClassification.end()){
					node->parentClassification[1]=parent;
				}
				else if(node->parentClassification.find(2) == node->parentClassification.end()){
					//						assert(node->parentClassification.find(2) == node->parentClassification.end());
					node->parentClassification[2]=parent;
				}
				else{
					LLVM_DEBUG(dbgs() << "Extra parent : " << parent->getIdx() << ",Nametype = " << parent->getNameType() << ",par_idx = " << node->parentClassification.size()+1 << "\n");
					node->parentClassification[node->parentClassification.size()+1]=parent;
				}
				continue;
			}

			if(dyn_cast<BranchInst>(ins)){
				if(node->parentClassification.find(1) == node->parentClassification.end()){
					node->parentClassification[1]=parent;
				}
				else{
					assert(node->parentClassification.find(2) == node->parentClassification.end());
					node->parentClassification[2]=parent;
				}
				continue;
			}

			if(parent->getNode()!=NULL){
				parentIns = parent->getNode();
				if(BranchInst* BRI = dyn_cast<BranchInst>(parentIns)){
					node->parentClassification[0]=parent;
					continue;
				}
				//					else if(CmpInst * CMPI = dyn_cast<CmpInst>(parentIns)){
				//						node->parentClassification[0]=parent;
				//						continue;
				//					}
			}
			else{
				if(parent->getNameType().compare("VRED")==0){
					if(node->parentClassification.find(1) == node->parentClassification.end())
						node->parentClassification[1]=parent;
					else
						node->parentClassification[2]=parent;
					continue;
				}
				if(parent->getNameType().compare("CTRLBrOR")==0){
					node->parentClassification[0]=parent;
					continue;
				}
				if(parent->getNameType().compare("PREDAND")==0){
					node->parentClassification[0]=parent;
					continue;
				}
				//					if(parent->getNameType().compare("GEPLEFTSHIFT")==0){
				//						assert(node->parentClassification.find(1)==node->parentClassification.end());
				//						node->parentClassification[1]=parent;
				//						continue;
				//					}
				if(parent->getNameType().compare("GEPLEFTSHIFT")==0 ||
				   parent->getNameType().compare("GEPSCALE")==0){
						assert(parent->getAncestors().size() == 1);
						dfgNode* parpar = parent->getAncestors()[0];
						if(OutLoopNodeMapReverse.find(parpar) != OutLoopNodeMapReverse.end()
							&& OutLoopNodeMapReverse[parpar]){
						parentIns = OutLoopNodeMapReverse[parpar];
					}
					else{
						assert(parpar->getNode());
						parentIns = parpar->getNode();
					}
					//						assert(parent->getAncestors().size()<3);
					//						node->parentClassification[1]=parent;
					//						continue;
				}
				if(parent->getNameType().compare("MASKAND")==0){
					assert(parent->getAncestors().size()==1);
					parentIns = parent->getAncestors()[0]->getNode();
				}
				if(parent->getNameType().compare("OutLoopLOAD")==0){
					parentIns = OutLoopNodeMapReverse[parent];
				}
				if(parent->getNameType().compare("CMERGE")==0){
					//						if(node->parentClassification.find(1) == node->parentClassification.end()){
					//							node->parentClassification[1]=parent;
					//						}
					//						else if(node->parentClassification.find(2) == node->parentClassification.end()){
					//	//						assert(node->parentClassification.find(2) == node->parentClassification.end());
					//							node->parentClassification[2]=parent;
					//						}
					//						else{
					//							LLVM_DEBUG(dbgs() << "Extra parent : " << parent->getIdx() << ",Nametype = " << parent->getNameType() << ",par_idx = " << node->parentClassification.size()+1 << "\n";
					//							node->parentClassification[node->parentClassification.size()+1]=parent;
					//						}
					//						continue;

					parentIns = cmergePHINodes[parent]->getNode();
					LLVM_DEBUG(dbgs() << "CMERGE Parent" << parent->getIdx() << "to non-nametype child=" << node->getIdx() << ".\n");
					LLVM_DEBUG(dbgs() << "parentIns = "); LLVM_DEBUG(parentIns->dump());
					LLVM_DEBUG(dbgs() << "node = "); LLVM_DEBUG(node->getNode()->dump());
					LLVM_DEBUG(dbgs() << "OpNumber = " << findOperandNumber(node, ins,parentIns) << "\n");

					assert(parentIns);
				}
				if(parent->getNameType().compare("SELECTPHI")==0){
					parentIns = selectPHIAncestorMap[parent]->getNode();
					LLVM_DEBUG(dbgs() << "SELECTPHI Parent" << parent->getIdx() << "to non-nametype child=" << node->getIdx() << ".\n");
					LLVM_DEBUG(dbgs() << "parentIns = "); LLVM_DEBUG(parentIns->dump());
					LLVM_DEBUG(dbgs() << "node = "); LLVM_DEBUG(node->getNode()->dump());
					LLVM_DEBUG(dbgs() << "OpNumber = " << findOperandNumber(node, ins,parentIns) << "\n");

					assert(parentIns);
				}
			}

			if(node->ancestorConditionaMap[parent] != UNCOND){
				node->parentClassification[0] = parent;
			}
			else{
				//Only ins!=NULL and non-PHI and non-BR will reach here
				// DMD: findOperandNumber can return 3 when the parent corresponds to
				// operand-index 2 of a 3-operand GEP instruction. Slot 3 is reserved
				// in the mapper to mean type_i1i2 (single-source, e.g. x*x), so
				// assigning a real register source to slot 3 causes the mapper to
				// discard the other input and produce wrong addresses. If the result is
				// slot 3 AND the node has an OutLoopLOAD ancestor (meaning there is a
				// second distinct register input), remap slot 3 to slot 1 so both
				// sources are encoded correctly.
				int _slot = findOperandNumber(node, ins, parentIns);
				if (_slot == 3) {
					bool _hasOLOADAnc = false;
					for (dfgNode* _anc : node->getAncestors())
						if (_anc->getNode() == nullptr &&
							_anc->getNameType().compare("OutLoopLOAD") == 0)
							{ _hasOLOADAnc = true; break; }
					if (_hasOLOADAnc)
						_slot = 1;
				}
				node->parentClassification[_slot]=parent;
			}

			// DMD: Set type I3 to handle single source nodes in mapper (e.g. x*x squaring).
			// This must only fire when the node genuinely has a single register input.
			// GEP ADD/SUB/MUL nodes with an OutLoopLOAD (OLOAD) ancestor have two distinct
			// register inputs: the OLOAD value (loop-invariant outer dimension) and an
			// LS-shifted inner-loop value. Triggering type_i1i2 in that case routes the same
			// LS result to both ALU I1 and I2, silently discarding the OLOAD input and
			// producing a wrong address (e.g. 2*(k<<5) instead of (k<<5)+j for a C[k][j] GEP).
			// insertshiftGEPsCorrect() populates OLOAD ancestors so parent_count >= 2 normally
			// guards this; the explicit hasOLOADAnc check below is a belt-and-suspenders guard.
			bool hasOLOADAnc = false;
			for (dfgNode* anc : node->getAncestors())
				if (anc->getNode() == nullptr &&
					anc->getNameType().compare("OutLoopLOAD") == 0) {
					hasOLOADAnc = true;
					break;
				}
			if(parent_count < 2 && !hasOLOADAnc && node->hasConstantVal()==false && (HyCUBEInsStrings[node->getFinalIns()].compare("MUL")==0 || HyCUBEInsStrings[node->getFinalIns()].compare("ADD")==0 || HyCUBEInsStrings[node->getFinalIns()].compare("SUB")==0)){
				node->parentClassification.erase(1);node->parentClassification.erase(2);
				node->parentClassification[3]=parent;
				LLVM_DEBUG(dbgs() <<"Has Const:" <<node->hasConstantVal()<<", Single source dest node:" << node->getIdx() << ", source node:" << parent->getIdx() << "\n");
			}


		}
	}

	return 0;
}
