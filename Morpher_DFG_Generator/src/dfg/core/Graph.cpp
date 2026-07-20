#include "DfgCoreInternal.h"

dfgNode *DFG::getEntryNode()
{
	if (NodeList.size() > 0)
	{
		return NodeList[0];
	}
	return NULL;
}

std::vector<dfgNode *> DFG::getNodes()
{
	return NodeList;
}

std::vector<Edge> DFG::getEdges()
{
	return edgeList;
}

void DFG::InsertNode(Instruction *Node)
{
	dfgNode *temp = new dfgNode(Node, this);
	temp->setIdx(NodeList.size());
	NodeList.push_back(temp);
}

//void DFG::InsertNode(dfgNode Node){
//	LLVM_DEBUG(dbgs() << "Inserted Node with Instruction : " << Node.getNode() << "\n";
//	Node.setIdx(NodeList.size());
//	NodeList.push_back(Node);
//}

void DFG::InsertEdge(Edge e)
{
	edgeList.push_back(e);
}

dfgNode *DFG::findNode(Instruction *I)
{
	for (int i = 0; i < NodeList.size(); i++)
	{
		if (I == NodeList[i]->getNode())
		{
			return NodeList[i];
		}
	}

	for (std::pair<Value *, dfgNode *> pair : OutLoopNodeMap)
	{
		if (pair.first == I)
		{
			return pair.second;
		}
	}

	return NULL;
}

Edge *DFG::findEdge(dfgNode *src, dfgNode *dest, CGRANode *goal)
{

	// 	if(src == dest){
	// //		assert(src->getPHIchildren().size()==1);
	// 		assert(goal!=NULL);
	// 		for(dfgNode* phiChild : src->getPHIchildren()){
	// 			if(phiChild->getMappedLoc() == goal){
	// 				dest = phiChild;
	// 			}
	// 		}
	// 		assert(dest!=src);
	// //		dest = src->getPHIchildren()[0];
	// 	}

	for (int i = 0; i < edgeList.size(); ++i)
	{
		if (edgeList[i].getSrc() == src && edgeList[i].getDest() == dest)
		{
			return &(edgeList[i]);
		}
	}

	LLVM_DEBUG(dbgs() << "edge is NULL, src = " << src->getIdx() << ",dest = " << dest->getIdx() << "\n");

	assert(false);
	return NULL;
}

std::vector<dfgNode *> DFG::getRoots()
{
	std::vector<dfgNode *> rootNodes;
	for (int i = 0; i < NodeList.size(); i++)
	{
		if (NodeList[i]->getChildren().size() == 0)
		{
			rootNodes.push_back(NodeList[i]);
		}
	}
	return rootNodes;
}

std::vector<dfgNode *> DFG::getLeafs(BasicBlock *BB)
{
	LLVM_DEBUG(dbgs() << "start getting the LeafNodes...!\n");
	std::vector<dfgNode *> leafNodes;
	for (int i = 0; i < NodeList.size(); i++)
	{
		//		if(NodeList[i]->getNode()->getParent() == BB){
		if (NodeList[i]->BB == BB)
		{

			if (NodeList[i]->getNode() != NULL)
			{
				if (dyn_cast<PHINode>(NodeList[i]->getNode()))
				{
					continue;
				}
			}

			if (NodeList[i]->getNameType().compare("OutLoopLOAD") == 0)
			{
			}
			else
			{
				leafNodes.push_back(NodeList[i]);
			}
		}
	}
	LLVM_DEBUG(dbgs() << "LeafNodes init done...!\n");

	for (int i = 0; i < NodeList.size(); i++)
	{
		//		if(NodeList[i]->getNode()->getParent() == BB){
		if (NodeList[i]->BB == BB)
		{

			if (NodeList[i]->getNameType().compare("OutLoopLOAD") == 0)
			{
				continue;
			}

			for (int j = 0; j < NodeList[i]->getChildren().size(); j++)
			{
				dfgNode *nodeToBeRemoved = NodeList[i]->getChildren()[j];
				if (nodeToBeRemoved != NULL)
				{
					//					LLVM_DEBUG(dbgs() << "LeafNodes : nodeToBeRemoved found...! : ";
					if (nodeToBeRemoved->getNode() == NULL)
					{
						//						LLVM_DEBUG(dbgs() << "NodeIdx:" << nodeToBeRemoved->getIdx() << "," << nodeToBeRemoved->getNameType() << "\n";
					}
					else
					{
						//						LLVM_DEBUG(dbgs() << "NodeIdx:" << nodeToBeRemoved->getIdx() << ",";
						LLVM_DEBUG(nodeToBeRemoved->getNode()->dump());
					}

					if (std::find(leafNodes.begin(), leafNodes.end(), nodeToBeRemoved) != leafNodes.end())
					{
						leafNodes.erase(std::remove(leafNodes.begin(), leafNodes.end(), nodeToBeRemoved));
					}
				}
			}
		}
	}
	LLVM_DEBUG(dbgs() << "got the LeafNodes...!\n");
	return leafNodes;
}

void DFG::connectBB()
{
	LLVM_DEBUG(dbgs() << "ConnectBB called!\n");

	std::map<dfgNode *, std::vector<dfgNode *>> BrSuccesors;
	std::map<dfgNode *, std::set<dfgNode *>> BrBackEdgeSuccessors;
	std::map<const BasicBlock *, dfgNode *> BBPredicate;
	dfgNode *temp;
	dfgNode *node;

	assert(NodeList.size() > 0);
	dfgNode firstNode = *NodeList[0];
	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8> Result;
	FindFunctionBackedges(*(firstNode.getNode()->getFunction()), Result);
	LLVM_DEBUG(dbgs() << "Number of Backedges = " << Result.size() << "\n");

	for (int i = 0; i < Result.size(); ++i)
	{
		LLVM_DEBUG(dbgs() << "Backedges .... :: \n");
		LLVM_DEBUG(dbgs() << "From : " << Result[i].first->getName());
		LLVM_DEBUG(dbgs() << ",To : " << Result[i].second->getName());
		LLVM_DEBUG(dbgs() << "\n");
	}

	std::vector<BasicBlock *> analysedBB;
	for (int i = 0; i < NodeList.size(); i++)
	{
		if (NodeList[i]->getNode() != NULL)
		{
			//			if(NodeList[i]->getNode()->getOpcode() == Instruction::Br){
			if (BranchInst *BI = dyn_cast<BranchInst>(NodeList[i]->getNode()))
			{
				LLVM_DEBUG(dbgs() << "$$$$$ This belongs to BB=" << NodeList[i]->getNode()->getParent()->getName() << "\n");

				BasicBlock *BB = NodeList[i]->getNode()->getParent();
				succ_iterator SI(succ_begin(BB)), SE(succ_end(BB));
				for (; SI != SE; ++SI)
				{
					BasicBlock *succ = *SI;
					LLVM_DEBUG(dbgs() << "$%$%$%$%$ successor Basic Blocks\n");
					LLVM_DEBUG(dbgs() << "Name=" << succ->getName() << "\n");
					LLVM_DEBUG(dbgs() << "$%$%$%$%$\n");

					std::vector<dfgNode *> succLeafs = this->getLeafs(succ);
					LLVM_DEBUG(dbgs() << "succLeafs.size = " << succLeafs.size() << "\n");

					std::pair<const BasicBlock *, const BasicBlock *> bbCouple(BB, succ);
					if (std::find(Result.begin(), Result.end(), bbCouple) != Result.end())
					{
						for (int j = 0; j < succLeafs.size(); j++)
						{
							LLVM_DEBUG(dbgs() << "Backedge from : " << NodeList[i]->getIdx() << ",To :" << succLeafs[j]->getIdx() << "\n");
							BrBackEdgeSuccessors[succLeafs[j]].insert(NodeList[i]);
						}
						//						 continue;
					}

					for (int j = 0; j < succLeafs.size(); j++)
					{
						BrSuccesors[succLeafs[j]].push_back(NodeList[i]);
					}
				}
			}
		}
	}

	//Connect the BBs here using the BrSuccesors Map
	std::map<dfgNode *, std::vector<dfgNode *>>::iterator it;
	std::vector<dfgNode *> workingSet;
	std::vector<dfgNode *> nextWorkingSet;
	int numberofbrs = 0;
	for (it = BrSuccesors.begin(); it != BrSuccesors.end(); it++)
	{
		numberofbrs = it->second.size();
		node = it->first;
		workingSet.clear();
		nextWorkingSet.clear();
		LLVM_DEBUG(dbgs() << "ConnectBB :: "
				<< "Init Round\n");

		if (BBPredicate.find(it->first->BB) != BBPredicate.end())
		{
			if (BrBackEdgeSuccessors[node].find(BBPredicate[node->BB]) ==
					BrBackEdgeSuccessors[node].end())
			{
				BBPredicate[node->BB]->addChildNode(node);
				node->addAncestorNode(BBPredicate[node->BB]);
			}
			else
			{
				BBPredicate[node->BB]->addPHIChildNode(node);
				node->addPHIAncestorNode(BBPredicate[node->BB]);
			}
			LLVM_DEBUG(dbgs() << "ConnectBB :: "
					<< "BB already done\n");
			continue;
		}

		if (numberofbrs == 1)
		{

			if (BrBackEdgeSuccessors[node].find(BrSuccesors[node][0]) ==
					BrBackEdgeSuccessors[node].end())
			{
				BrSuccesors[node][0]->addChildNode(node);
				node->addAncestorNode(BrSuccesors[node][0]);
			}
			else
			{
				BrSuccesors[node][0]->addPHIChildNode(node);
				node->addPHIAncestorNode(BrSuccesors[node][0]);
			}

			BBPredicate[node->BB] = BrSuccesors[node][0];
			continue;
		}

		for (int i = 0; i < numberofbrs - 1; i = i + 2)
		{
			temp = new dfgNode(this);
			temp->setNameType("CTRLBrOR");
			temp->setIdx(NodeList.size());
			temp->BB = node->BB;
			NodeList.push_back(temp);

			if (BrBackEdgeSuccessors[node].find(BrSuccesors[node][i]) ==
					BrBackEdgeSuccessors[node].end())
			{
				temp->addAncestorNode(BrSuccesors[node][i]);
				BrSuccesors[node][i]->addChildNode(temp);
			}
			else
			{
				temp->addPHIAncestorNode(BrSuccesors[node][i]);
				BrSuccesors[node][i]->addPHIChildNode(temp);
			}

			if (BrBackEdgeSuccessors[node].find(BrSuccesors[node][i + 1]) ==
					BrBackEdgeSuccessors[node].end())
			{
				temp->addAncestorNode(BrSuccesors[node][i + 1]);
				BrSuccesors[node][i + 1]->addChildNode(temp);
			}
			else
			{
				temp->addPHIAncestorNode(BrSuccesors[node][i + 1]);
				BrSuccesors[node][i + 1]->addPHIChildNode(temp);
			}
			workingSet.push_back(temp);
		}

		if (numberofbrs % 2 == 1)
		{
			workingSet.push_back(BrSuccesors[node][numberofbrs - 1]);
		}
		LLVM_DEBUG(dbgs() << "ConnectBB :: "
				<< "Rest Rounds\n");
		while (workingSet.size() > 1)
		{
			LLVM_DEBUG(dbgs() << "ConnectBB :: "
					<< "workingSet.size() = " << workingSet.size() << "\n");
			for (int i = 0; i < workingSet.size() - 1; i = i + 2)
			{
				temp = new dfgNode(this);
				temp->setIdx(NodeList.size());
				NodeList.push_back(temp);
				temp->setNameType("CTRLBrOR");
				temp->BB = node->BB;
				LLVM_DEBUG(dbgs() << "ConnectBB :: "
						<< "workingSet[i+1] = " << workingSet[i + 1]->getIdx() << "\n");

				if (BrBackEdgeSuccessors[node].find(workingSet[i]) ==
						BrBackEdgeSuccessors[node].end())
				{
					workingSet[i]->addChildNode(temp);
					temp->addAncestorNode(workingSet[i]);
				}
				else
				{
					workingSet[i]->addPHIChildNode(temp);
					temp->addPHIAncestorNode(workingSet[i]);
				}

				if (BrBackEdgeSuccessors[node].find(workingSet[i + 1]) ==
						BrBackEdgeSuccessors[node].end())
				{
					workingSet[i + 1]->addChildNode(temp);
					temp->addAncestorNode(workingSet[i + 1]);
				}
				else
				{
					workingSet[i + 1]->addPHIChildNode(temp);
					temp->addPHIAncestorNode(workingSet[i + 1]);
				}

				nextWorkingSet.push_back(temp);
			}
			if (workingSet.size() % 2 == 1)
			{
				nextWorkingSet.push_back(workingSet[workingSet.size() - 1]);
			}

			workingSet.clear();
			for (int i = 0; i < nextWorkingSet.size(); ++i)
			{
				workingSet.push_back(nextWorkingSet[i]);
			}
			nextWorkingSet.clear();
		}

		assert(workingSet.size() == 1);

		if (BrBackEdgeSuccessors[node].find(workingSet[0]) ==
				BrBackEdgeSuccessors[node].end())
		{
			workingSet[0]->addChildNode(node);
			node->addAncestorNode(workingSet[0]);
		}
		else
		{
			workingSet[0]->addPHIChildNode(node);
			node->addAncestorNode(workingSet[0]);
		}

		BBPredicate[node->BB] = workingSet[0];
	}
	LLVM_DEBUG(dbgs() << "ConnectBB DONE! \n");
}

//WIP
int DFG::handlePHINodeFanIn()
{
	dfgNode *node;
	dfgNode *ancestor;
	dfgNode *temp;
	std::vector<dfgNode *> phiNodes;
	std::vector<dfgNode *> workingSet;
	std::vector<dfgNode *> nextWorkingSet;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getNode() != NULL)
		{
			if (node->getNode()->getOpcode() == Instruction::PHI)
			{
				phiNodes.push_back(node);
			}
		}
	}

	if (phiNodes.empty())
	{
		return 0;
	}

	//Remove the current connection
	for (int i = 0; i < phiNodes.size(); ++i)
	{
		node = phiNodes[i];

		if (node->getAncestors().size() <= 2)
		{
			continue;
		}

		for (int j = 0; j < node->getAncestors().size(); ++j)
		{
			ancestor = node->getAncestors()[j];
			workingSet.push_back(node->getAncestors()[j]);

			ancestor->removeChild(node);
			node->removeAncestor(ancestor);
			removeEdge(findEdge(ancestor, node));
		}

		while (workingSet.size() > 1)
		{
			LLVM_DEBUG(dbgs() << "handlePHINodeFanIn :: "
					<< "workingSet.size() = " << workingSet.size() << "\n");
			for (int i = 0; i < workingSet.size() - 1; i = i + 2)
			{
				temp = new dfgNode(this);
				temp->setIdx(NodeList.size());
				NodeList.push_back(temp);
				temp->setNameType("SELECTPHI");
				workingSet[i]->addChildNode(temp);
				LLVM_DEBUG(dbgs() << "handlePHINodeFanIn :: "
						<< "workingSet[i+1] = " << workingSet[i + 1]->getIdx() << "\n");
				workingSet[i + 1]->addChildNode(temp);
				temp->addAncestorNode(workingSet[i]);
				temp->addAncestorNode(workingSet[i + 1]);
				nextWorkingSet.push_back(temp);
			}
			if (workingSet.size() % 2 == 1)
			{
				nextWorkingSet.push_back(workingSet[workingSet.size() - 1]);
			}

			workingSet.clear();
			for (int i = 0; i < nextWorkingSet.size(); ++i)
			{
				workingSet.push_back(nextWorkingSet[i]);
			}
			nextWorkingSet.clear();
		}
		assert(workingSet.size() == 1);
		workingSet[0]->addChildNode(node);
		node->addAncestorNode(workingSet[0]);
	}
	return 0;
}

void DFG::printXML()
{
	std::string fileName = name + "_dfg.xml";
	xmlFile.open(fileName.c_str());
	printDFGInfo();
	printHeaderTag("DFG-information");
	printOPs(1); //depth = 1
	printEdges(1);
	printFooterTag("DFG-information");
	xmlFile.close();
}

void DFG::printInEdges(dfgNode *node, int depth)
{
	printHeaderTag("In-edge-number", depth);
	xmlFile << node->getAncestors().size();
	printFooterTag("In-edge-number", depth);

	printHeaderTag("In-edges", depth);
	for (int i = 0; i < node->getAncestors().size(); ++i)
	{
		printHeaderTag("Edge", depth + 1);

		printHeaderTag("ID", depth + 2);
		//		xmlFile << node->getAncestors()[i] << "to" << node->getNode();
		Edge *e = findEdge(node->getAncestors()[i], node);
		assert(e);

		if (xmlEdgeIdxMap.find(e) == xmlEdgeIdxMap.end())
		{
			xmlEdgeIdxMap[e] = xmlEdgeCount;
			xmlEdgeCount++;
		}

		xmlFile << xmlEdgeIdxMap[e];
		printFooterTag("ID", depth + 2);

		printFooterTag("Edge", depth + 1);
	}

	printFooterTag("In-edges", depth);
}

void DFG::printOutEdges(dfgNode *node, int depth)
{
	printHeaderTag("Out-edge-number", depth);
	xmlFile << node->getChildren().size();
	printFooterTag("Out-edge-number", depth);

	printHeaderTag("Out-edges", depth);

	for (int i = 0; i < node->getChildren().size(); ++i)
	{
		printHeaderTag("Edge", depth + 1);

		printHeaderTag("ID", depth + 2);
		//		xmlFile << node->getNode() << "to" << node->getChildren()[i];
		//		xmlFile << findEdge(node,node->getChildren()[i])->getID() ;
		Edge *e = findEdge(node, node->getChildren()[i]);
		assert(e);
		if (xmlEdgeIdxMap.find(e) == xmlEdgeIdxMap.end())
		{
			xmlEdgeIdxMap[e] = xmlEdgeCount;
			xmlEdgeCount++;
		}
		xmlFile << xmlEdgeIdxMap[e];
		printFooterTag("ID", depth + 2);

		printFooterTag("Edge", depth + 1);
	}

	printFooterTag("Out-edges", depth);
}

void DFG::printOP(dfgNode *node, int depth)
{
	assert(xmlFile.is_open());
	printHeaderTag("OP", depth);

	printHeaderTag("ID", depth + 1);
	//	xmlFile << (node->getNode());
	xmlFile << node->getIdx();
	printFooterTag("ID", depth + 1);

	printHeaderTag("OP-type", depth + 1);
	//	xmlFile << "NORMAL"; //TODO :: Hardcoded to be Normal, fix the hardcoding

	if (node->getNode() != NULL)
	{
		switch (isMemoryOp(node))
		{
		case LOAD:
			xmlFile << "LOAD";
			break;
		case STORE:
			xmlFile << "STORE";
			break;
		default:
			xmlFile << "NORMAL";
			break;
		}
	}
	else
	{
		if (node->getNameType().compare("OutLoopLOAD") == 0)
		{
			xmlFile << "LOAD";
		}
		else if (node->getNameType().compare("OutLoopSTORE") == 0)
		{
			xmlFile << "STORE";
		}
		else
		{
			xmlFile << "NORMAL";
		}
		//xmlFile << node->getNameType();
	}

	printFooterTag("OP-type", depth + 1);

	printHeaderTag("Cycles", depth + 1);
	xmlFile << 1; //TODO :: Remove the hardcoding
	printFooterTag("Cycles", depth + 1);

	printInEdges(node, depth + 1);
	printOutEdges(node, depth + 1);

	printFooterTag("OP", depth);
}

void DFG::printOPs(int depth)
{
	assert(xmlFile.is_open());
	printHeaderTag("OPs", depth);

	printHeaderTag("OP-number", depth + 1);
	xmlFile << NodeList.size();
	printFooterTag("OP-number", depth + 1);

	for (int i = 0; i < NodeList.size(); ++i)
	{
		printOP(NodeList[i], depth + 1);
	}
	printFooterTag("OPs", depth);
}

void DFG::printDFGInfo()
{
	assert(xmlFile.is_open());
	xmlFile << "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << std::endl;
}

void DFG::printHeaderTag(std::string tagName, int depth)
{
	assert(xmlFile.is_open());
	xmlFile << std::endl;
	for (int i = 0; i < depth; ++i)
	{
		xmlFile << "\t";
	}
	xmlFile << "<" << tagName << ">";
}

void DFG::printFooterTag(std::string tagName, int depth)
{
	assert(xmlFile.is_open());
	xmlFile << std::endl;
	for (int i = 0; i < depth; ++i)
	{
		xmlFile << "\t";
	}
	xmlFile << "</" << tagName << ">";
}

void DFG::printEdges(int depth)
{
	Edge *e;
	int edgeCount = 0;
	std::vector<Edge> localEdgeList;

	assert(xmlFile.is_open());
	printHeaderTag("EDGEs", depth);

	printHeaderTag("Edge-number", depth);

	//	for (int i = 0; i < edgeList.size(); ++i) {
	//		e = &(edgeList[i]);
	//		if(e->getType() == EDGE_TYPE_DATA){
	//			edgeCount++;
	//		}
	//	}
	//	xmlFile << edgeCount;
	assert(xmlEdgeIdxMap.size() == xmlEdgeCount);
	xmlFile << xmlEdgeIdxMap.size();

	printFooterTag("Edge-number", depth);

	// Currently only printing DATA dependency edges only
	//	for (int i = 0; i < edgeList.size(); ++i) {
	//		e = &(edgeList[i]);
	//		if(e->getType() == EDGE_TYPE_DATA){
	//			printEdge(e,depth);
	//		}
	//	}

	for (std::pair<Edge *, int> pair : xmlEdgeIdxMap)
	{
		printEdge(pair.first, depth);
	}

	printFooterTag("EDGEs", depth);
}

void DFG::renumber()
{
	for (int i = 0; i < NodeList.size(); ++i)
	{
		NodeList[i]->setIdx(i);
	}

	for (int i = 0; i < edgeList.size(); ++i)
	{
		edgeList[i].setID(i);
	}
}

void DFG::printEdge(Edge *e, int depth)
{
	assert(xmlFile.is_open());
	printHeaderTag("EDGE", depth);

	printHeaderTag("ID", depth + 1);
	xmlFile << xmlEdgeIdxMap[e];
	printFooterTag("ID", depth + 1);

	printHeaderTag("Attribute", depth + 1);
	xmlFile << 1; //God knows what this is
	printFooterTag("Attribute", depth + 1);

	printHeaderTag("Start-OP", depth + 1);
	//	xmlFile << e->getSrc();
	xmlFile << e->getSrc()->getIdx();
	printFooterTag("Start-OP", depth + 1);

	printHeaderTag("End-OP", depth + 1);
	//	xmlFile << e->getDest();
	xmlFile << e->getDest()->getIdx();
	printFooterTag("End-OP", depth + 1);

	printFooterTag("EDGE", depth);
}

void DFG::addMemRecDepEdges(DependenceInfo *DI)
{
	std::vector<dfgNode *> memNodes;
	dfgNode *nodePtr;
	Instruction *Ins;
	std::ofstream log;

	static int count = 0;
	std::string Filename = (NodeList[0]->getNode()->getFunction()->getName() + "_L" + std::to_string(count++) + "addMemRecDepEdges.log").str();
	log.open(Filename.c_str());
	log << "Started...\n";

	LLVM_DEBUG(dbgs() << "&&&&&&&&&&&&&&&&&&&&Recurrence search started.....!\n");

	//Create a list of memory instructions
	for (int i = 0; i < NodeList.size(); ++i)
	{
		nodePtr = NodeList[i];
		Ins = dyn_cast<Instruction>(nodePtr->getNode());
		if (!Ins)
			return;

		LoadInst *Ld = dyn_cast<LoadInst>(nodePtr->getNode());
		StoreInst *St = dyn_cast<StoreInst>(nodePtr->getNode());

		if (!St && !Ld)
			continue;
		if (Ld && !Ld->isSimple())
			return;
		if (St && !St->isSimple())
			return;
		LLVM_DEBUG(dbgs() << "ID=" << nodePtr->getIdx() << " ,");
		LLVM_DEBUG(nodePtr->getNode()->dump());
		memNodes.push_back(nodePtr);
	}

	log << "addMemRecDepEdges : Found " << memNodes.size() << "Loads and Stores to analyze\n";

	for (int i = 0; i < memNodes.size(); ++i)
	{
		for (int j = i; j < memNodes.size(); ++j)
		{
			std::vector<char> Dep;
			Instruction *Src = dyn_cast<Instruction>(memNodes[i]->getNode());
			Instruction *Des = dyn_cast<Instruction>(memNodes[j]->getNode());

			if (Src == Des)
				continue;

			if (isa<LoadInst>(Src) && isa<LoadInst>(Des))
				continue;

			if (auto D = DI->depends(Src, Des, true))
			{
				log << "addMemRecDepEdges :"
						<< "Found Dependency between Src=" << memNodes[i]->getIdx() << " Des=" << memNodes[j]->getIdx() << "\n";

				if (D->isFlow())
				{
					// TODO: Handle Flow dependence.Check if it is sufficient to populate
					// the Dependence Matrix with the direction reversed.
					log << "addMemRecDepEdges :"
							<< "Flow dependence not handled\n";
					continue;
					//					  return;
				}
				if (D->isAnti())
				{
					log << "Found Anti dependence \n";

					this->findNode(Src)->addChild(Des, EDGE_TYPE_LDST);
					this->findNode(Des)->addAncestor(Src);

					unsigned Levels = D->getLevels();
					char Direction;
					for (unsigned II = 1; II <= Levels; ++II)
					{
						const SCEV *Distance = D->getDistance(II);

						const SCEVConstant *SCEVConst = dyn_cast_or_null<SCEVConstant>(Distance);
						if (SCEVConst)
						{
							const ConstantInt *CI = SCEVConst->getValue();
							if (CI->isNegative())
							{
								Direction = '<';
							}
							else if (CI->isZero())
							{
								Direction = '=';
							}
							else
							{
								Direction = '>';
							}
							log << SCEVConst->getAPInt().abs().getZExtValue() << std::endl;
							Dep.push_back(Direction);
						}
						else if (D->isScalar(II))
						{
							Direction = 'S';
							Dep.push_back(Direction);
						}
						else
						{
							unsigned Dir = D->getDirection(II);
							if (Dir == Dependence::DVEntry::LT || Dir == Dependence::DVEntry::LE)
							{
								Direction = '<';
							}
							else if (Dir == Dependence::DVEntry::GT || Dir == Dependence::DVEntry::GE)
							{
								Direction = '>';
							}
							else if (Dir == Dependence::DVEntry::EQ)
							{
								Direction = '=';
							}
							else
							{
								Direction = '*';
							}
							Dep.push_back(Direction);
						}
					}
					//					while (Dep.size() != Level) {
					//					  Dep.push_back('I');
					//					}
				}

				for (int k = 0; k < Dep.size(); ++k)
				{
					log << Dep[k];
				}
				log << "\n";
			}
		}
	}

	log.close();
}

void DFG::findMaxRecDist()
{
	dfgNode *node;
	int recDist;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		for (int j = 0; j < node->getRecAncestors().size(); ++j)
		{
			recDist = node->getASAPnumber() - node->getRecAncestors()[j]->getASAPnumber();
			if (maxRecDist < recDist)
			{
				maxRecDist = recDist;
			}
		}

		for (dfgNode *phiParent : node->getPHIancestors())
		{
			const BasicBlock *phiBB = phiParent->BB;
			const BasicBlock *nodeBB = node->BB;

			SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8> Result;
			FindFunctionBackedges(*(nodeBB->getParent()), Result);

			std::pair<const BasicBlock *, const BasicBlock *> bbCouple(phiBB, nodeBB);
			if (std::find(Result.begin(), Result.end(), bbCouple) != Result.end())
			{
				int phiDist = phiParent->getASAPnumber() - node->getASAPnumber();
				if (maxRecDist < phiDist)
				{
					maxRecDist = phiDist;
				}
			}
		}
	}
}

void DFG::addMemRecDepEdgesNew(DependenceInfo *DI)
{
	std::vector<dfgNode *> memNodes;
	dfgNode *nodePtr;
	Instruction *Ins;
	std::ofstream log;
	int RecDist;

	static int count = 0;
	std::string Filename = (NodeList[0]->getNode()->getFunction()->getName() + "_L" + std::to_string(count++) + "addMemRecDepEdges.log").str();
	log.open(Filename.c_str());
	log << "Started...\n";

	LLVM_DEBUG(dbgs() << "&&&&&&&&&&&&&&&&&&&&Recurrence search started.....!\n");

	//Create a list of memory instructions
	for (int i = 0; i < NodeList.size(); ++i)
	{
		nodePtr = NodeList[i];

		if (nodePtr->getNode() == NULL)
		{
			continue;
		}
		Ins = dyn_cast<Instruction>(nodePtr->getNode());
		if (!Ins)
			return;

		LoadInst *Ld = dyn_cast<LoadInst>(nodePtr->getNode());
		StoreInst *St = dyn_cast<StoreInst>(nodePtr->getNode());

		if (!St && !Ld)
			continue;
		if (Ld && !Ld->isSimple())
			return;
		if (St && !St->isSimple())
			return;
		LLVM_DEBUG(dbgs() << "ID=" << nodePtr->getIdx() << " ,");
		LLVM_DEBUG(nodePtr->getNode()->dump());
		memNodes.push_back(nodePtr);
	}

	log << "addMemRecDepEdges : Found " << memNodes.size() << "Loads and Stores to analyze\n";

	for (int i = 0; i < memNodes.size(); ++i)
	{
		for (int j = i; j < memNodes.size(); ++j)
		{
			std::vector<char> Dep;
			Instruction *Src = dyn_cast<Instruction>(memNodes[i]->getNode());
			Instruction *Des = dyn_cast<Instruction>(memNodes[j]->getNode());

			if (Src == Des)
				continue;

			if (isa<LoadInst>(Src) && isa<LoadInst>(Des))
				continue;

			if (auto D = DI->depends(Src, Des, true))
			{
				log << "addMemRecDepEdges :"
						<< "Found Dependency between Src=" << memNodes[i]->getIdx() << " Des=" << memNodes[j]->getIdx() << "\n";
				LLVM_DEBUG(dbgs() << "addMemRecDepEdges :"
						<< "Found Dependency between Src=" << memNodes[i]->getIdx() << " Des=" << memNodes[j]->getIdx() << "\n");

				if (D->isLoopIndependent())
				{
					// This pass emits loop-carried recurrence edges only. A same-
					// iteration memory flow is already represented by the regular
					// data path (for example LOAD -> ADD -> STORE in GEMM).
					log << "Loop independent; not a recurrence edge\n";
					LLVM_DEBUG(dbgs() << "Loop Independent; skipping recurrence edge.\n");
					continue;
				}
				LLVM_DEBUG(dbgs() << "Loop Dependent.\n");

				LLVM_DEBUG(outs() << "Levels = " << D->getLevels() << "\n");
				LLVM_DEBUG(D->dump(outs()));

				char Direction;
				for (int i = 1; i <= D->getLevels(); i++)
				{
					const SCEV *Distance = D->getDistance(i);
					const SCEVConstant *SCEVConst = dyn_cast_or_null<SCEVConstant>(Distance);
					if (SCEVConst)
					{
						const ConstantInt *CI = SCEVConst->getValue();
						if (CI->isNegative())
							Direction = '<';
						else if (CI->isZero())
							Direction = '=';
						else
							Direction = '>';
					}
					else if (D->isScalar(i))
					{
						Direction = 'S';
					}
					else
					{
						unsigned Dir = D->getDirection(i);
						if (Dir == Dependence::DVEntry::LT || Dir == Dependence::DVEntry::LE)
							Direction = '<';
						else if (Dir == Dependence::DVEntry::GT || Dir == Dependence::DVEntry::GE)
							Direction = '>';
						else if (Dir == Dependence::DVEntry::EQ)
							Direction = '=';
						else
							Direction = '*';
					}

					LLVM_DEBUG(dbgs() << "\tlevel=" << i << ",dir=" << Direction << "\n");
				}

				if (D->isAnti())
				{
					// TODO: Handle Anit dependence.Check if it is sufficient to populate
					// the Dependence Matrix with the direction reversed.
					log << "addMemRecDepEdges :"
							<< "Anti dependence not handled\n";
					continue;
					//					  return;
				}

				if (D->isFlow())
				{
					log << "Found Flow dependence \n";

					if (std::find(BBSuccBasicBlocks[Src->getParent()].begin(), BBSuccBasicBlocks[Src->getParent()].end(), Des->getParent()) == BBSuccBasicBlocks[Src->getParent()].end())
					{
						continue;
					}

					//							this->findNode(Src)->addRecChild(Des,EDGE_TYPE_LDST);
					//							this->findNode(Des)->addRecAncestor(Src);
					std::string depType;
					if (D->isFlow())
					{
						depType = "FLOW";
					}
					else
					{
						assert(D->isAnti());
						depType = "ANTI";
					}

					if (D->isConfused())
						continue;

					LLVM_DEBUG(dbgs() << "adding recurrence relation!\n");
					this->findNode(Des)->addRecChild(Src, depType, EDGE_TYPE_LDST);
					this->findNode(Src)->addRecAncestor(Des, depType);

					//							RecDist = this->findNode(Des)->getASAPnumber() - this->findNode(Src)->getASAPnumber();
					//							if(maxRecDist < RecDist){
					//								maxRecDist = RecDist;
					//							}

					unsigned Levels = D->getLevels();
					char Direction;

					for (unsigned II = 1; II <= Levels; ++II)
					{
						const SCEV *Distance = D->getDistance(II);

						const SCEVConstant *SCEVConst = dyn_cast_or_null<SCEVConstant>(Distance);
						if (SCEVConst)
						{
							const ConstantInt *CI = SCEVConst->getValue();
							if (CI->isNegative())
							{
								Direction = '<';
							}
							else if (CI->isZero())
							{
								Direction = '=';
							}
							else
							{
								Direction = '>';
							}
							log << SCEVConst->getAPInt().abs().getZExtValue() << std::endl;
							Dep.push_back(Direction);
						}
						else if (D->isScalar(II))
						{
							Direction = 'S';
							Dep.push_back(Direction);
						}
						else
						{
							unsigned Dir = D->getDirection(II);
							if (Dir == Dependence::DVEntry::LT || Dir == Dependence::DVEntry::LE)
							{
								Direction = '<';
							}
							else if (Dir == Dependence::DVEntry::GT || Dir == Dependence::DVEntry::GE)
							{
								Direction = '>';
							}
							else if (Dir == Dependence::DVEntry::EQ)
							{
								Direction = '=';
							}
							else
							{
								Direction = '*';
							}
							Dep.push_back(Direction);
						}
					}
					//					while (Dep.size() != Level) {
					//					  Dep.push_back('I');
					//					}
				}

				for (int k = 0; k < Dep.size(); ++k)
				{
					log << Dep[k];
				}
				log << "\n";
			}
		}
	}

	log.close();
	//		assert(false);
	LLVM_DEBUG(dbgs() << "&&&&&&&&&&&&&&&&&&&&Recurrence search done.....!\n");
}

void DFG::addMemDepEdges(MemoryDependenceResults *MD)
{

	assert(NodeList.size() > 0);
	dfgNode firstNode = *NodeList[0];
	SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 1> Result;
	FindFunctionBackedges(*(firstNode.getNode()->getFunction()), Result);

	Instruction *it;
	MemDepResult mRes;
	SmallVector<NonLocalDepResult, 1> result;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		it = NodeList[i]->getNode();
		if (it->mayReadOrWriteMemory())
		{
			mRes = MD->getDependency(it);

			LLVM_DEBUG(dbgs() << "Dependent :");
			LLVM_DEBUG(it->dump());

				if (mRes.isNonLocal())
				{
					if (isa<CallBase>(it))
					{
					}
					else
					{
						LLVM_DEBUG(dbgs() << "****** GET NON LOCAL MEM DEPENDENCIES ******\n");

					MD->getNonLocalPointerDependency(it, result);

					for (int j = 0; j < result.size(); ++j)
					{
						if (result[j].getResult().getInst() != NULL)
						{

							if ((result[j].getResult().getInst()->getOpcode() == Instruction::Load) && (it->getOpcode() == Instruction::Load))
							{
							}
							else
							{

								std::pair<const BasicBlock *, const BasicBlock *> bbCouple(result[j].getResult().getInst()->getParent(), it->getParent());
								if (std::find(Result.begin(), Result.end(), bbCouple) != Result.end())
								{
									continue;
								}

								LLVM_DEBUG(result[j].getResult().getInst()->dump());
								this->findNode(result[j].getResult().getInst())->addChild(it, EDGE_TYPE_LDST);
								NodeList[i]->addAncestor(result[j].getResult().getInst());
							}
						}
					}

					LLVM_DEBUG(dbgs() << "****** DONE NON LOCAL MEM DEPENDENCIES ******\n");
				}
			}

			if (mRes.getInst() != NULL)
			{

				if ((mRes.getInst()->getOpcode() == Instruction::Load) && (it->getOpcode() == Instruction::Load))
				{
					//					continue;
				}
				else
				{
					this->findNode(mRes.getInst())->addChild(it, EDGE_TYPE_LDST);
					NodeList[i]->addAncestor(mRes.getInst());
				}
			}
		}
	}
}

int DFG::removeEdge(Edge *e)
{
	Edge *edgePtr;
	edgePtr = findEdge(e->getSrc(), e->getDest());
	if (edgePtr == NULL)
	{
		return -1;
	}

	for (int i = 0; i < edgeList.size(); ++i)
	{
		if (edgePtr == &(edgeList[i]))
		{
			edgeList.erase(edgeList.begin() + i);
		}
	}

	return 1;
}

int DFG::removeNode(dfgNode *n)
{
	dfgNode *nodePtr;
	nodePtr = findNode(n->getNode());
	if (nodePtr == NULL)
	{
		return -1;
	}

	dfgNode *nodeTmp;
	dfgNode *nodeTmp2;
	Edge *edgeTmp;

	//Treat the ancestors
	for (int i = 0; i < nodePtr->getAncestors().size(); ++i)
	{
		nodeTmp = nodePtr->getAncestors()[i];
		assert(nodeTmp->removeChild(nodePtr->getNode()) != -1);

		for (int j = 0; j < nodePtr->getChildren().size(); ++j)
		{
			nodeTmp->addChild(nodePtr->getChildren()[j]->getNode()); //TODO : Generalize for types of children
			nodeTmp2 = nodePtr->getChildren()[j];

			assert(nodeTmp2->removeAncestor(nodePtr->getNode()) != -1);
			nodeTmp2->addAncestor(nodePtr->getAncestors()[i]->getNode());
		}

		edgeTmp = findEdge(nodePtr->getAncestors()[i], nodePtr);
		assert(removeEdge(edgeTmp) != -1);
	}

	//Treat the children
	for (int i = 0; i < nodePtr->getChildren().size(); ++i)
	{
		nodeTmp = nodePtr->getChildren()[i];
		assert(nodeTmp->removeAncestor(nodePtr->getNode()) != -1);

		for (int j = 0; j < nodePtr->getAncestors().size(); ++j)
		{
			nodeTmp->addAncestor(nodePtr->getAncestors()[j]->getNode());
			nodeTmp2 = nodePtr->getAncestors()[j];

			assert(nodeTmp2->removeChild(nodePtr->getNode()) != -1);
			nodeTmp2->addChild(nodePtr->getChildren()[i]->getNode());
		}

		edgeTmp = findEdge(nodePtr, nodePtr->getChildren()[i]);
		assert(removeEdge(edgeTmp) != -1);
	}

	//	NodeList.erase(std::remove(NodeList.begin(),NodeList.end(),*n),NodeList.end());

	for (int i = 0; i < NodeList.size(); ++i)
	{
		if (nodePtr == NodeList[i])
		{
			NodeList.erase(NodeList.begin() + i);
		}
	}

	return 1;
}

void DFG::removeAlloc()
{
	std::unordered_set<dfgNode*> rn;

	for(dfgNode* n : NodeList){
		if(n->getNode() && isa<AllocaInst>(n->getNode())){
			rn.insert(n);
		}
		else if(OutLoopNodeMapReverse.find(n) != OutLoopNodeMapReverse.end()){
			if(isa<AllocaInst>(OutLoopNodeMapReverse[n])){
				rn.insert(n);
			}
		}
	}

	for(dfgNode* n : rn){
		for(dfgNode* chl : n->getChildren()){
			chl->removeAncestor(n);
		}
		for(dfgNode* anc : n->getAncestors()){
			anc->removeChild(n);
		}
		NodeList.erase(std::remove(NodeList.begin(), NodeList.end(), n), NodeList.end());
	}
}

void DFG::traverseDFS(dfgNode *startNode, int dfsCount)
{
	//	//Assumption should be a connected DFG
	//	assert(getRoots().size()==1);
	//
	//	for (int i = 0; i < startNode->getChildren(); ++i) {
	//
	//	}
}

void DFG::traverseBFS(dfgNode *node, int ASAPlevel)
{

	if (node->getNode())
	{
		LLVM_DEBUG(node->getNode()->dump());
	}

	dfgNode *child;
	for (int i = 0; i < node->getChildren().size(); ++i)
	{

		if (maxASAPLevel < ASAPlevel)
		{
			maxASAPLevel = ASAPlevel;
		}

		child = node->getChildren()[i];
		if (child->getASAPnumber() < ASAPlevel)
		{
			child->setASAPnumber(ASAPlevel);
			traverseBFS(child, ASAPlevel + 1);
		}
	}

	for (int i = 0; i < node->getRecChildren().size(); ++i)
	{

		if (maxASAPLevel < ASAPlevel)
		{
			maxASAPLevel = ASAPlevel;
		}

		child = node->getRecChildren()[i];
		if (child->getASAPnumber() < ASAPlevel)
		{
			child->setASAPnumber(ASAPlevel);
			traverseBFS(child, ASAPlevel + 1);
		}
	}
}

void DFG::traverseInvBFS(dfgNode *node, int ALAPlevel)
{
	dfgNode *ancestor;
	for (int i = 0; i < node->getAncestors().size(); ++i)
	{
		ancestor = node->getAncestors()[i];
		if (ancestor->getALAPnumber() < ALAPlevel)
		{
			ancestor->setALAPnumber(ALAPlevel);
			traverseInvBFS(ancestor, ALAPlevel + 1);
		}
	}

	for (int i = 0; i < node->getRecAncestors().size(); ++i)
	{
		ancestor = node->getRecAncestors()[i];
		if (ancestor->getALAPnumber() < ALAPlevel)
		{
			ancestor->setALAPnumber(ALAPlevel);
			traverseInvBFS(ancestor, ALAPlevel + 1);
		}
	}
}

void DFG::scheduleASAP()
{
	std::vector<dfgNode *> leafs;
	leafs = getLeafs();
	int nodesVisited = 0;

	for (int i = 0; i < leafs.size(); ++i)
	{
		leafs[i]->setASAPnumber(0);
		traverseBFS(leafs[i], 1);
	}
	LLVM_DEBUG(dbgs() << "scheduleASAP DONE!\n");
}

void DFG::scheduleALAP()
{
	dfgNode *node;

	int currASAPnumber = -1;
	std::vector<dfgNode *> roots;
	//	for (int i = 0; i < NodeList.size(); ++i) {
	//		node = &(NodeList[i]);
	////		assert(node->getASAPnumber() != -1);
	//		if(node->getASAPnumber() > currASAPnumber){
	//			currASAPnumber = node->getASAPnumber();
	//		}
	//	}

	//	for (int i = 0; i < NodeList.size(); ++i) {
	//		if(node->getASAPnumber() == currASAPnumber){
	//			roots.push_back(node);
	//		}
	//	}

	roots = getRoots();

	for (int i = 0; i < roots.size(); ++i)
	{
		roots[i]->setALAPnumber(0);
		traverseInvBFS(roots[i], 1);
	}

	for (int i = 0; i < NodeList.size(); ++i)
	{
		NodeList[i]->setALAPnumber(maxASAPLevel - NodeList[i]->getALAPnumber());
	}

	LLVM_DEBUG(dbgs() << "scheduleALAP DONE!\n");
}

void DFG::balanceASAPALAP()
{
	dfgNode *node;

	std::map<dfgNode *, int> newASAPVal;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		int ASAP_ALAP_diff = node->getALAPnumber() - node->getASAPnumber();
		ASAP_ALAP_diff = ASAP_ALAP_diff / 2;
		newASAPVal[node] = node->getASAPnumber() + ASAP_ALAP_diff;
	}

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getChildren().empty())
		{
			node->setASAPnumber(newASAPVal[node]);
		}
		else
		{
			int minASAP = 0;
			for (dfgNode *child : node->getChildren())
			{
				if (minASAP < newASAPVal[child])
				{
					minASAP = newASAPVal[child];
				}
			}

			if (newASAPVal[node] < minASAP)
			{
				node->setASAPnumber(newASAPVal[node]);
			}
			else
			{
				node->setASAPnumber(minASAP - 1);
				newASAPVal[node] = minASAP - 1;
			}
		}
	}

	//	for (int i = 0; i < NodeList.size(); ++i) {
	//		node = NodeList[i];
	//		int ASAP_ALAP_diff = node->getALAPnumber() - node->getASAPnumber();
	//		ASAP_ALAP_diff = ASAP_ALAP_diff/2;
	//		if(ASAP_ALAP_diff > 2){ //Hack for compressed sensing
	//			node->setASAPnumber(node->getASAPnumber() + ASAP_ALAP_diff + 2);
	//		}
	//		else if(ASAP_ALAP_diff>0){
	//			node->setASAPnumber(node->getASAPnumber() + ASAP_ALAP_diff);
	//		}
	////		node->setASAPnumber(node->getALAPnumber());
	//	}
}

std::vector<dfgNode *> DFG::getLeafs()
{
	std::vector<dfgNode *> leafNodes;
	for (int i = 0; i < NodeList.size(); i++)
	{
		if (NodeList[i]->getAncestors().size() == 0)
		{
			leafNodes.push_back(NodeList[i]);
		}
	}
	return leafNodes;
}
