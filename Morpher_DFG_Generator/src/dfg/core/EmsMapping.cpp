#include "DfgCoreInternal.h"

void DFG::traverseCriticalPath(dfgNode *node, int level)
{
	//	dfgNode* temp;
	//	int i;
	//	for (i = 0; i < node->getChildren().size(); ++i) {
	//		temp = findNode(node->getChildren()[i]);
	//		if(temp->getASAPnumber() == temp->getALAPnumber()) {
	//
	//		}
	//	}
}

void DFG::CreateSchList()
{
	dfgNode *temp;

	LLVM_DEBUG(dbgs() << "#################Begin :: The Schedule List#################\n");

	//	for (int i = 0; i < NodeList.size(); ++i) {
	//		temp = &NodeList[i];
	//		LLVM_DEBUG(dbgs() << "NodeIdx=" << temp->getIdx() << ", ASAP =" << temp->getASAPnumber() << ", ALAP =" << temp->getALAPnumber() << "\n";
	//	}
	//	LLVM_DEBUG(dbgs() << "Done\n";

	std::sort(NodeList.begin(), NodeList.end(), ScheduleOrder());
	LLVM_DEBUG(dbgs() << "CreateSchList::Done sorting...\n");
	for (int i = 0; i < NodeList.size(); ++i)
	{
		temp = NodeList[i];
		temp->setSchIdx(i);

		LLVM_DEBUG(dbgs() << "NodeIdx=" << temp->getIdx() <<  ", NameType=" << temp->getNameType() << ", ASAP =" << temp->getASAPnumber() << ", ALAP =" << temp->getALAPnumber() << ", CONST VAL =" << (temp->hasConstantVal()? temp->getConstantVal():0) << "\n");
	}
	LLVM_DEBUG(dbgs() << "#################End :: The Schedule List#################\n");
}

std::vector<ConnectedCGRANode> DFG::searchCandidates(CGRANode *mappedLoc, dfgNode *node, std::vector<std::pair<Instruction *, int>> *candidateNumbers)
{
	std::vector<ConnectedCGRANode> candidates = mappedLoc->getConnectedNodes();
	eraseAlreadyMappedNodes(&candidates);
	candidateNumbers->push_back(std::make_pair(node->getAncestors()[0]->getNode(), candidates.size()));
	dfgNode *temp;
	//		candidateNumbers[node->getAncestors()[0]] = candidates.size();

	std::vector<ConnectedCGRANode> candidates2;
	std::vector<ConnectedCGRANode> candidates3;
	bool matchFound = false;

	for (int i = 0; i < node->getAncestors().size(); ++i)
	{
		if (mappedLoc->getmappedDFGNode()->getNode() != node->getAncestors()[i]->getNode())
		{
			temp = findNode(node->getAncestors()[i]->getNode());
			assert(temp->getMappedLoc() != NULL);
			candidates2 = getConnectedCGRANodes(temp);
			//					candidates2 = temp->getMappedLoc()->getConnectedNodes();
			//					eraseAlreadyMappedNodes(&candidates2);

			candidateNumbers->push_back(std::make_pair(node->getAncestors()[i]->getNode(), candidates2.size()));

			for (int j = 0; j < candidates.size(); ++j)
			{
				for (int k = 0; k < candidates2.size(); ++k)
				{
					if (candidates[j].node == candidates2[k].node)
					{
						candidates[j].cost = candidates[j].cost + candidates2[k].cost;
						matchFound = true;
						break;
					}
				}
				if (matchFound)
				{
					candidates3.push_back(candidates[j]);
				}
				matchFound = false;
			}
			candidates = candidates3;
		}
	}
	eraseAlreadyMappedNodes(&candidates);
	return candidates;
}

void DFG::eraseAlreadyMappedNodes(std::vector<ConnectedCGRANode> *candidates)
{
	std::vector<ConnectedCGRANode>::iterator it = candidates->begin();

	while (it != candidates->end())
	{
		if (it->node->getmappedDFGNode() != NULL)
		{
			//			LLVM_DEBUG(dbgs() << "eraseAlreadyMappedNodes :: Already mapped = (" << it->node->getT() << ","
			//																	  << it->node->getY() << ","
			//																	  << it->node->getX() << ")\n";
			it = candidates->erase(it);
		}
		else
		{
			++it;
		}
	}
}

void DFG::backTrack(int nodeSeq)
{
	dfgNode *temp = NodeList[nodeSeq];
	dfgNode *anc;
	//	LLVM_DEBUG(dbgs() << "Backtrack : NodeSeq=" << nodeSeq << "\n";

	if (temp->getMappedLoc() != NULL)
	{
		//		LLVM_DEBUG(dbgs() << "(t,y,x) = (" << temp->getMappedLoc()->getT() << "," << temp->getMappedLoc()->getY() << "," << temp->getMappedLoc()->getX() << "\n";
		temp->getMappedLoc()->setMappedDFGNode(NULL);
		temp->setMappedLoc(NULL);
	}

	for (int i = 0; i < temp->getAncestors().size(); ++i)
	{
		anc = temp->getAncestors()[i];

		std::vector<CGRANode *>::iterator it = anc->getRoutingLocs()->begin();

		while (it != anc->getRoutingLocs()->end())
		{
			//			LLVM_DEBUG(dbgs() << "BackTrack :: erasing routing=(" << (*it)->getT() << ","
			//													   << (*it)->getY() << ","
			//													   << (*it)->getX() << ")\n";
			(*it)->setMappedDFGNode(NULL);
			it = anc->getRoutingLocs()->erase(it);
		}
	}
}

void DFG::ExpandCandidatesAddingRoutingNodes(
		std::vector<std::pair<Instruction *, int>> *candidateNumbers)
{

	dfgNode *temp;
	std::vector<ConnectedCGRANode> candidates2;
	std::vector<std::pair<Instruction *, int>> candidateNumbersTemp;
	std::sort(candidateNumbers->begin(), candidateNumbers->end(), ValueComparer());

	//	int conMat[currCGRA->getMII()*currCGRA->getYdim()*currCGRA->getXdim()][currCGRA->getMII()*currCGRA->getYdim()*currCGRA->getXdim()];
	std::vector<std::vector<int>> conMat;

	for (int i = 0; i < currCGRA->getMII() * currCGRA->getYdim() * currCGRA->getXdim(); ++i)
	{
		std::vector<int> temp;
		for (int j = 0; j < currCGRA->getMII() * currCGRA->getYdim() * currCGRA->getXdim(); ++j)
		{
			temp.push_back(0);
		}
		conMat.push_back(temp);
	}

	std::vector<ConnectedCGRANode> phyCand;

	for (int t = 0; t < currCGRA->getMII(); ++t)
	{
		for (int y = 0; y < currCGRA->getYdim(); ++y)
		{
			for (int x = 0; x < currCGRA->getXdim(); ++x)
			{
				conMat[getConMatIdx(t, y, x)][getConMatIdx(t, y, x)] = 1;

				phyCand = currCGRA->getCGRANode(t, y, x)->getConnectedNodes();
				for (int i = 0; i < phyCand.size(); ++i)
				{
					if (phyCand[i].node->getmappedDFGNode() == NULL)
					{
						conMat[getConMatIdx(t, y, x)][getConMatIdx(phyCand[i].node->getT(), phyCand[i].node->getY(), phyCand[i].node->getX())] = 1;
					}
				}
			}
		}
	}

	for (int i = 0; i < candidateNumbers->size(); ++i)
	{
		temp = findNode((*candidateNumbers)[i].first);

		candidates2 = getConnectedCGRANodes(temp);
		std::sort(candidates2.begin(), candidates2.end(), CostComparer());

		for (int j = 0; j < candidates2.size(); ++j)
		{

			//					LLVM_DEBUG(dbgs() << "candidates2 = (" << candidates2[j].node->getT() << ","
			//							   	   	   	   	    << candidates2[j].node->getY() << ","
			//												<< candidates2[j].node->getX() << ")\n";
		}
	}
}

std::vector<ConnectedCGRANode> DFG::getConnectedCGRANodes(dfgNode *node)
{
	assert(node->getMappedLoc() != NULL);

	std::vector<ConnectedCGRANode> candidates;
	std::vector<ConnectedCGRANode> candidates2;
	candidates = node->getMappedLoc()->getConnectedNodes();
	bool found = false;

	int routingCost;
	for (int i = 0; i < node->getRoutingLocs()->size(); ++i)
	{
		found = false;
		routingCost = (*node->getRoutingLocs())[i]->getT() - node->getMappedLoc()->getT();
		if (routingCost < 0)
		{
			routingCost = routingCost + currCGRA->getMII();
		}

		candidates2 = (*node->getRoutingLocs())[i]->getConnectedNodes();

		for (int j = 0; j < candidates2.size(); ++j)
		{
			candidates2[j].cost = candidates2[j].cost + routingCost;

			found = false;
			for (int k = 0; k < candidates.size(); ++k)
			{
				if (candidates2[j].node == candidates[k].node)
				{
					found = true;
				}
			}

			if (!found)
			{
				candidates.push_back(candidates2[j]);
			}
		}
	}
	eraseAlreadyMappedNodes(&candidates);
	return candidates;
}

int DFG::getConMatIdx(int t, int y, int x)
{
	return t * currCGRA->getYdim() * currCGRA->getXdim() + y * currCGRA->getXdim() + x;
}

std::vector<ConnectedCGRANode> DFG::FindCandidateCGRANodes(dfgNode *node)
{
	dfgNode *temp;
	std::vector<ConnectedCGRANode> candidates;
	std::vector<std::pair<Instruction *, int>> candidateNumbers;
	std::vector<std::pair<Instruction *, int>> candidateNumbersLvl2;

	assert(currCGRA != NULL);

	if (node->getAncestors().size() == 0)
	{
		for (int t = 0; t < currCGRA->getMII(); ++t)
		{
			for (int y = 0; y < currCGRA->getYdim(); ++y)
			{
				for (int x = 0; x < currCGRA->getXdim(); ++x)
				{
					if (currCGRA->getCGRANode(t, y, x)->getmappedDFGNode() == NULL)
					{
						ConnectedCGRANode tempCGRANode(currCGRA->getCGRANode(t, y, x), 0, "startNodes");
						candidates.push_back(tempCGRANode);
					}
				}
			}
		}
		return candidates;
	}
	else if (node->getAncestors().size() == 1)
	{
		temp = node->getAncestors()[0];
		assert(temp->getMappedLoc() != NULL);

		candidates = getConnectedCGRANodes(temp);
		//		candidates = temp->getMappedLoc()->getConnectedNodes();

		//		for (int i = 0; i < candidates.size(); ++i) {
		//			if(candidates[i].node->getmappedDFGNode() != NULL){
		//				candidates.erase(candidates.begin() + i);
		//			}
		//		}
		eraseAlreadyMappedNodes(&candidates);

		return candidates;
	}
	else
	{ //(node->getAncestors().size() > 1
		temp = node->getAncestors()[0];

		if (temp->getMappedLoc() == NULL)
		{
			LLVM_DEBUG(dbgs() << "Unmapped ancestor : " << temp->getIdx() << "\n");
		}

		assert(temp->getMappedLoc() != NULL);

		if (temp->getMappedLoc()->getmappedDFGNode() == NULL)
		{
			LLVM_DEBUG(dbgs() << "Unmapped ancestor : " << temp->getIdx() << "\n");
		}

		assert(temp->getMappedLoc()->getmappedDFGNode() != NULL);
		candidateNumbers.clear();
		candidates = searchCandidates(temp->getMappedLoc(), node, &candidateNumbers);

		//		LLVM_DEBUG(dbgs() << "FindCandidateCGRANodes :: candidates.size()=" << candidates.size() << "\n";

		std::vector<ConnectedCGRANode> candidates2;

		if (candidates.size() == 0)
		{
			std::sort(candidateNumbers.begin(), candidateNumbers.end(), ValueComparer());
			for (int i = 0; i < candidateNumbers.size(); ++i)
			{
				temp = findNode(candidateNumbers[i].first);
				candidates2 = getConnectedCGRANodes(temp);
				//				candidates2 = temp->getMappedLoc()->getConnectedNodes();
				//				eraseAlreadyMappedNodes(&candidates2);
				std::sort(candidates2.begin(), candidates2.end(), CostComparer());

				for (int j = 0; j < candidates2.size(); ++j)
				{
					//					LLVM_DEBUG(dbgs() << "candidates2 = (" << candidates2[j].node->getT() << ","
					//							   	   	   	   	    << candidates2[j].node->getY() << ","
					//												<< candidates2[j].node->getX() << ")\n";
				}

				//search one-level with routing nodes
				for (int j = 0; j < candidates2.size(); ++j)
				{
					candidates2[j].node->setMappedDFGNode(temp);
					candidates = searchCandidates(candidates2[j].node, node, &candidateNumbersLvl2);
					if (candidates.size() == 0)
					{
						candidates2[j].node->setMappedDFGNode(NULL);
					}
					else
					{
						//						LLVM_DEBUG(dbgs() << "FindCandidateCGRANodes :: Routing node added=(" << candidates2[j].node->getT() << ","
						//																				   << candidates2[j].node->getY() << ","
						//																				   << candidates2[j].node->getX() << ")\n";
						temp->getRoutingLocs()->push_back(candidates2[j].node);
						break;
					}
				}
				if (candidates.size() != 0)
				{
					break;
				}
			}
		}

		if (candidates.size() == 0)
		{
			LLVM_DEBUG(dbgs() << "No Candidates found for NODE_ID:" << node->getIdx() << "\n");
			LLVM_DEBUG(dbgs() << "Need to backtrack...\n");
			return candidates;
		}

		std::sort(candidates.begin(), candidates.end(), CostComparer());
		return candidates;
	}
}

void DFG::MapCGRA_EMS(int XDim, int YDim, std::string mapfileName)
{
	mappingOutFile.open(mapfileName.c_str());
	clock_t begin = clock();
	int MII = std::ceil((float)NodeList.size() / ((float)XDim * (float)YDim));
	findMaxRecDist();
	//	conMat = getConMat();
	conMatArr.push_back(getConMat());
	printConMat(conMatArr[0]);

	LLVM_DEBUG(dbgs() << "MapCGRAsa:: Resource Constrained MII = " << MII << "\n");
	LLVM_DEBUG(dbgs() << "MapCGRAsa:: Recurrence Constrained MII = " << getMaxRecDist() << "\n");
	mappingOutFile << "MapCGRAsa:: Number of nodes = " << NodeList.size() << "\n";
	mappingOutFile << "MapCGRAsa:: Resource Constrained MII = " << MII << "\n";
	mappingOutFile << "MapCGRAsa:: Recurrence Constrained MII = " << getMaxRecDist() << "\n";

	MII = std::max(MII, getMaxRecDist());

	astar = new AStar(&mappingOutFile, MII, this);

	//Sanity Check
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getAncestors().size() > 5)
		{
			LLVM_DEBUG(dbgs() << "Cannot map applications that have Fan in nodes more than 5\n");
			return;
		}
	}

	while (1)
	{
		if (MapCGRA_EMS_ASAPLevel(MII, XDim, YDim))
		{
			clock_t end = clock();
			double elapsed_time = double(end - begin) / CLOCKS_PER_SEC;
			mappingOutFile << "Duration :: " << elapsed_time << "\n";
			mappingOutFile << "MapCGRAsa :: Mapping success with MII = " << MII << "\n";

			std::map<CGRANode *, std::vector<CGRAEdge>> *cgraEdgesPtr = currCGRA->getCGRAEdges();
			std::vector<CGRAEdge> connections;
			int count = 0;

			mappingOutFile << "Reg Connections Available after use :: \n";

			for (int t = 0; t < currCGRA->getMII(); ++t)
			{
				for (int y = 0; y < currCGRA->getYdim(); ++y)
				{
					for (int x = 0; x < currCGRA->getXdim(); ++x)
					{
						connections = (*cgraEdgesPtr)[currCGRA->getCGRANode(t, y, x)];
						for (int c = 0; c < connections.size(); ++c)
						{
							if ((connections[c].Dst->getY() == y) && (connections[c].Dst->getX() == x))
							{
								count++;
							}
						}
						mappingOutFile << "(" << t << "," << y << "," << x << ")"
								<< "=" << count << "\n";
						count = 0;
					}
				}
			}
			break;
		}

		delete (currCGRA);
		clearMapping();
		MII++;
	}
}

bool DFG::MapCGRA_EMS_ASAPLevel(int MII, int XDim, int YDim)
{
	currCGRA = new CGRA(MII, XDim, YDim, REGS_PER_NODE);

	LLVM_DEBUG(dbgs() << "STARTING MAPASAP with MII = " << MII << "with maxASAPLevel = " << maxASAPLevel << "\n");
	mappingOutFile << "STARTING MAPASAP with MII = " << MII << "with maxASAPLevel = " << maxASAPLevel << "\n";

	std::map<CGRANode *, std::vector<CGRAEdge>>::iterator edgeIt;
	for (edgeIt = currCGRA->getCGRAEdges()->begin();
			edgeIt != currCGRA->getCGRAEdges()->end();
			edgeIt++)
	{
		mappingOutFile << "DEBUG:: Node=" << edgeIt->first->getName() << ", NumberOfEdges=" << edgeIt->second.size() << std::endl;
	}

	//	currCGRA->getCGRAEdges()

	//	std::map<dfgNode*,std::vector<CGRANode*> > nodeDestMap;
	std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> nodeDestMap;
	std::map<CGRANode *, std::vector<dfgNode *>> destNodeMap;
	std::map<dfgNode *, std::map<CGRANode *, int>> nodeDestCostMap;

	std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>>::iterator nodeDestMapIt;
	std::map<CGRANode *, std::vector<dfgNode *>>::iterator destNodeMapIt;

	std::vector<dfgNode *> currLevelNodes;
	dfgNode *node;
	int mappedRealTime;
	dfgNode *phiChild;
	dfgNode *parent;
	CGRANode *parentExt;

	CGRANode *end;
	Port endPort;

	//----Data Structures for Sorting the destination nodes based on routing --- (1)
	std::map<std::pair<CGRANode *, Port>, std::pair<CGRANode *, Port>> cameFrom;
	std::map<CGRANode *, int> costSoFar;
	CGRANode *cnode;
	std::pair<CGRANode *, CGRANode *> path;
	int cost;

	std::vector<nodeWithCost> nodesWithCost;
	//	std::vector<nodeWithCost> globalNodesWithCost;
	// End of --- (1)

	for (int level = 0; level <= maxASAPLevel; ++level)
	{

		globalNodesWithCost.clear();
		currLevelNodes.clear();
		nodeDestMap.clear();
		destNodeMap.clear();
		nodeDestCostMap.clear();

		LLVM_DEBUG(dbgs() << "level = " << level << "\n");

		for (int j = 0; j < NodeList.size(); ++j)
		{
			node = NodeList[j];

			if (node->getASAPnumber() == level)
			{
				currLevelNodes.push_back(node);
			}
		}

		LLVM_DEBUG(dbgs() << "numOfNodes = " << currLevelNodes.size() << "\n");

		for (int i = 0; i < currLevelNodes.size(); ++i)
		{
			node = currLevelNodes[i];
			int ll = 0;
			int el = INT32_MAX;
			for (int j = 0; j < node->getAncestors().size(); ++j)
			{
				parent = node->getAncestors()[j];

				//every parent should be mapped
				if (parent->getMappedLoc() == NULL)
				{
					LLVM_DEBUG(dbgs() << "parent :: nodeIdx=" << parent->getIdx() << ", ASAP=" << parent->getASAPnumber() << "\n");
					//					parent->getNode()->dump();
				}
				assert(parent->getMappedLoc() != NULL);

				if (parent->getmappedRealTime() > ll)
				{
					if (parent->getmappedRealTime() % MII != parent->getMappedLoc()->getT())
					{
						LLVM_DEBUG(dbgs() << "getMappedRealTime assertion is going to fail!\n");
						LLVM_DEBUG(dbgs() << "parent->getmappedRealTime()%MII = " << parent->getmappedRealTime() % MII << "\n");
						LLVM_DEBUG(dbgs() << "parent->getMappedLoc()->getT() = " << parent->getMappedLoc()->getT() << "\n");
					}

					assert(parent->getmappedRealTime() % MII == parent->getMappedLoc()->getT());
					//						ll = parent->getMappedLoc()->getT();
					ll = parent->getmappedRealTime();
				}

				//					if(parent->getmappedRealTime() < el){
				//						assert( parent->getmappedRealTime()%MII == parent->getMappedLoc()->getT() );
				//						el = parent->getMappedLoc()->getT();
				////						el = parent->getmappedRealTime();
				//					}
			}

			if (!node->getRecAncestors().empty())
			{
				LLVM_DEBUG(dbgs() << "RecAnc for Node" << node->getIdx());
				mappingOutFile << "RecAnc for Node" << node->getIdx();
			}

			for (int j = 0; j < node->getRecAncestors().size(); ++j)
			{
				parent = node->getRecAncestors()[j];

				LLVM_DEBUG(dbgs() << " (Id=" << parent->getIdx() << ",rt=" << parent->getmappedRealTime() << ",t=" << parent->getMappedLoc()->getT() << "),");

				mappingOutFile << " (Id=" << parent->getIdx() << ",rt=" << parent->getmappedRealTime() << ",t=" << parent->getMappedLoc()->getT() << "),";

				if (parent->getmappedRealTime() < el)
				{
					assert(parent->getmappedRealTime() % MII == parent->getMappedLoc()->getT());
					el = parent->getmappedRealTime();
				}
			}

			if (!node->getRecAncestors().empty())
			{
				LLVM_DEBUG(dbgs() << "\n");
				mappingOutFile << "\n";
				el = el % MII;
			}

			for (int var = 0; var < MII; ++var)
			{

				if (ll + 1 == el)
				{ //this is only set if reccurence ancestors are found.
					mappingOutFile << "MapASAPLevel breaking since reccurence ancestor found\n";
					break;
				}

				if (!node->getPHIchildren().empty())
				{

					assert(node->getPHIchildren().size() == 1);
					phiChild = node->getPHIchildren()[0];
					assert(phiChild->getMappedLoc() != NULL);
					int y = phiChild->getMappedLoc()->getY();
					int x = phiChild->getMappedLoc()->getX();

					if (currCGRA->getCGRANode((ll + 1) % MII, y, x)->getmappedDFGNode() == NULL)
					{
						nodeDestMap[node].push_back(std::make_pair(currCGRA->getCGRANode((ll + 1) % MII, y, x), (ll + 1)));
						destNodeMap[currCGRA->getCGRANode((ll + 1) % MII, y, x)].push_back(node);
					}
				}
				else
				{

					for (int y = 0; y < YDim; ++y)
					{
						for (int x = 0; x < XDim; ++x)
						{
							if (currCGRA->getCGRANode((ll + 1) % MII, y, x)->getmappedDFGNode() == NULL)
							{
								nodeDestMap[node].push_back(std::make_pair(currCGRA->getCGRANode((ll + 1) % MII, y, x), (ll + 1)));
								destNodeMap[currCGRA->getCGRANode((ll + 1) % MII, y, x)].push_back(node);
							}
						}
					}
				}

				ll = (ll + 1);
			}
			LLVM_DEBUG(dbgs() << "MapASAPLevel:: nodeIdx=" << node->getIdx() << " ,Possible Dests = " << nodeDestMap[node].size() << "\n");
		}

		LLVM_DEBUG(dbgs() << "MapASAPLevel:: Finding dests are done!\n");

		//Sorting the nodeDestMap based routing/affinity costs
		for (nodeDestMapIt = nodeDestMap.begin(); nodeDestMapIt != nodeDestMap.end(); nodeDestMapIt++)
		{
			node = nodeDestMapIt->first;
			nodesWithCost.clear();
			for (int i = 0; i < nodeDestMapIt->second.size(); ++i)
			{
				cnode = nodeDestMapIt->second[i].first;
				mappedRealTime = nodeDestMapIt->second[i].second;

				cost = 0;
				for (int j = 0; j < node->getAncestors().size(); ++j)
				{
					parent = node->getAncestors()[j];
					parentExt = parent->getMappedLoc();
					//					parentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 1)%(currCGRA->getMII()),parent->getMappedLoc()->getY(),parent->getMappedLoc()->getX());
					path = std::make_pair(parentExt, cnode);

					//Calculate cost for the path
					end = astar->AStarSearchEMS(*(currCGRA->getCGRAEdges()), parentExt, cnode, &cameFrom, &costSoFar, &endPort);
					cost += costSoFar[cnode];
				}
				nodesWithCost.push_back(nodeWithCost(node, cnode, cost, mappedRealTime));
			}
			std::sort(nodesWithCost.begin(), nodesWithCost.end(), LessThanNodeWithCost());
			globalNodesWithCost.push_back(nodesWithCost[0]);
			nodeDestMap[node].clear();
			for (std::vector<nodeWithCost>::iterator it = nodesWithCost.begin();
					it != nodesWithCost.end();
					it++)
			{
				nodeDestMap[node].push_back(std::make_pair(it->cnode, it->mappedRealTime));
			}
			assert(nodeDestMap[node].size() != 0);
			//TODO : Implement affinity cost
		}

		std::sort(globalNodesWithCost.begin(), globalNodesWithCost.end(), LessThanNodeWithCost());

		deadEndReached = false;
		if (!MAPCGRA_EMS_MultDest(&nodeDestMap, &destNodeMap, globalNodesWithCost.begin(), *(currCGRA->getCGRAEdges()), 0))
		{
			return false;
		}

	} // End of ASAP level iterations
	return true;
}

bool DFG::MAPCGRA_EMS_MultDest(std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> *nodeDestMap,
		std::map<CGRANode *, std::vector<dfgNode *>> *destNodeMap,
		//							   std::map<dfgNode*,std::vector< std::pair<CGRANode*,int> > >::iterator it,
		const std::vector<nodeWithCost>::iterator it,
		std::map<CGRANode *, std::vector<CGRAEdge>> cgraEdges,
		int index)
{

	std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> localNodeDestMap = *nodeDestMap;
	std::map<CGRANode *, std::vector<dfgNode *>> localdestNodeMap = *destNodeMap;

	dfgNode *node;
	dfgNode *otherNode;
	dfgNode *parent;
	CGRANode *cnode;
	std::pair<CGRANode *, int> cnodePair;
	CGRANode *parentExt;
	//	std::vector< std::pair<CGRANode*,int> > possibleDests;
	std::vector<dfgNode *> parents;
	//	std::vector<std::pair<CGRANode*, CGRANode*> > paths;
	std::vector<CGRANode *> dests;
	std::vector<std::pair<CGRANode *, CGRANode *>> pathsNotRouted;
	bool success = false;
	//	int idx = index;

	//	CGRA localCGRA = *inCGRA;
	std::map<CGRANode *, std::vector<CGRAEdge>> localCGRAEdges = cgraEdges;

	node = it->node;
	//	possibleDests = it->second;

	//	if(node == NULL){
	//		LLVM_DEBUG(dbgs() << "Node is NULL...\n";
	//	}else{
	//		LLVM_DEBUG(dbgs() << "Node = \n";
	//		node->getNode()->dump();
	//	}

	//	LLVM_DEBUG(dbgs() << "MapMultiDestRec : Procesing NodeIdx = " << node->getIdx() << " ,PossibleDests = " << (*nodeDestMap)[node].size() << "\n";

	LLVM_DEBUG(dbgs() << "EMSMapMultiDestRec : Procesing NodeIdx = " << node->getIdx());
	LLVM_DEBUG(dbgs() << ", PossibleDests = " << (*nodeDestMap)[node].size());
	LLVM_DEBUG(dbgs() << ", MII = " << currCGRA->getMII());
	LLVM_DEBUG(dbgs() << ", currASAPLevel = " << node->getASAPnumber());
	LLVM_DEBUG(dbgs() << ", NodeProgress = " << index + 1 << "/" << nodeDestMap->size());
	LLVM_DEBUG(dbgs() << "\n");

	for (int i = 0; i < (*nodeDestMap)[node].size(); ++i)
	{
		success = false;

		if ((*nodeDestMap)[node][i].first->getmappedDFGNode() == NULL)
		{
			LLVM_DEBUG(dbgs() << "Possible Dest = "
					<< "(" << (*nodeDestMap)[node][i].first->getT() << ","
					<< (*nodeDestMap)[node][i].first->getY() << ","
					<< (*nodeDestMap)[node][i].first->getX() << "), Index=" << i + 1 << "/" << (*nodeDestMap)[node].size() << "\n");
			cnode = (*nodeDestMap)[node][i].first;
			cnodePair = (*nodeDestMap)[node][i];
			for (int j = 0; j < node->getAncestors().size(); ++j)
			{
				parent = node->getAncestors()[j];
				//				parentExt = currCGRA->getCGRANode(cnode->getT(),parent->getMappedLoc()->getY(),parent->getMappedLoc()->getX());
				parentExt = parent->getMappedLoc();
				//				parentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 1)%(currCGRA->getMII()),parent->getMappedLoc()->getY(),parent->getMappedLoc()->getX());

				//				LLVM_DEBUG(dbgs() << "Path = "
				//					   << "(" << parentExt->getT() << ","
				//					   	   	  << parentExt->getY() << ","
				//							  << parentExt->getX() << ") to ";
				//
				//				LLVM_DEBUG(dbgs() << "(" << cnode->getT() << ","
				//					   	   	  << cnode->getY() << ","
				//							  << cnode->getX() << ")\n";

				//				paths.push_back(std::make_pair(parentExt,cnode));
				dests.push_back(cnode);
				parents.push_back(parent);
			}

			localCGRAEdges = cgraEdges;
			mappingOutFile << "nodeIdx=" << node->getIdx() << ", placed=(" << cnode->getT() << "," << cnode->getY() << "," << cnode->getX() << ")"
					<< "\n";
			//			astar->EMSRoute(parents,paths,&localCGRAEdges,&pathsNotRouted);
			astar->EMSRoute(node, parents, dests, &localCGRAEdges, &pathsNotRouted, &deadEndReached);
			dests.clear();
			if (!pathsNotRouted.empty())
			{
				for (int j = 0; j < parents.size(); ++j)
				{
					for (int k = 0; k < parents[j]->getRoutingLocs()->size(); ++k)
					{
						(*parents[j]->getRoutingLocs())[k]->setMappedDFGNode(NULL);
					}
					parents[j]->getRoutingLocs()->clear();
				}
				mappingOutFile << "routing failed, clearing edges\n";
				LLVM_DEBUG(dbgs() << "all paths are not routed.\n");
				pathsNotRouted.clear();
				if (deadEndReached)
				{
					return false;
				}
				continue;
			}
			mappingOutFile << "routing success, keeping the current edges\n";
			pathsNotRouted.clear();

			for (int j = 0; j < localdestNodeMap[cnode].size(); ++j)
			{
				otherNode = localdestNodeMap[cnode][j];
				localNodeDestMap[otherNode].erase(std::remove(localNodeDestMap[otherNode].begin(), localNodeDestMap[otherNode].end(), cnodePair), localNodeDestMap[otherNode].end());
			}

			//			it->second.clear();
			//			it->second.push_back(cnodePair);
			localdestNodeMap[cnode].clear();

			LLVM_DEBUG(dbgs() << "Placed = "
					<< "(" << cnode->getT() << "," << cnode->getY() << "," << cnode->getX() << ")\n");
			node->setMappedLoc(cnode);
			cnode->setMappedDFGNode(node);
			node->setMappedRealTime(cnodePair.second);

			std::vector<nodeWithCost>::iterator itlocal;
			itlocal = it;
			itlocal++;
			//			it++;

			//			idx++;

			if (index + 1 < nodeDestMap->size())
			{
				if (!EMSSortNodeDest(&localNodeDestMap, localCGRAEdges, index + 1))
				{
					LLVM_DEBUG(dbgs() << "&& EMSSortNodeDest Done and Some nodes does not have destination retrying...\n");
					success = false;
				}
				else
				{
					itlocal = globalNodesWithCost.begin() + index + 1;
					LLVM_DEBUG(dbgs() << "&& EMSSortNodeDest Done \n");
					success = MAPCGRA_EMS_MultDest(
							&localNodeDestMap,
							&localdestNodeMap,
							itlocal,
							localCGRAEdges,
							index + 1);
				}
			}
			else
			{
				LLVM_DEBUG(dbgs() << "nodeDestMap end reached..\n");
				*nodeDestMap = localNodeDestMap;
				*destNodeMap = localdestNodeMap;
				currCGRA->setCGRAEdges(localCGRAEdges);
				success = true;
			}

			if (deadEndReached)
			{
				success = false;
				break;
			}

			if (success)
			{
				(*nodeDestMap)[node].clear();
				(*nodeDestMap)[node].push_back(cnodePair);
				break;
			}
			else
			{
				LLVM_DEBUG(dbgs() << "MapMultiDestRec : fails next possible destination\n");
				node->setMappedLoc(NULL);
				cnode->setMappedDFGNode(NULL);
				mappingOutFile << std::to_string(index + 1) << " :: mapping failed, therefore trying mapping again for index=" << std::to_string(index) << "\n";
			}
		}
	}

	if (success)
	{
	}
	return success;
}

void DFG::clearMapping()
{
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		node->setMappedLoc(NULL);
		node->setMappedRealTime(-1);
		node->getRoutingLocs()->clear();
	}
}

TreePath DFG::createTreePath(dfgNode *parent, CGRANode *dest)
{
	TreePath tp;
	dfgNode *child;
	CGRANode *ParentExt;
	CGRANode *cnode;
	int MII = currCGRA->getMII();

	//Initial node, AKA the source
	assert(parent->getMappedLoc() != NULL);

	if (parent->getMappedLoc()->getPEType() == MEM)
	{
		ParentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 2) % MII, parent->getMappedLoc()->getY(), parent->getMappedLoc()->getX());
		tp.sourceSCpathTdimLengths[ParentExt] = 2;
	}
	else
	{
		ParentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 1) % MII, parent->getMappedLoc()->getY(), parent->getMappedLoc()->getX());
		tp.sourceSCpathTdimLengths[ParentExt] = 1;
	}
	tp.sources.push_back(ParentExt);
	tp.sourcePorts[ParentExt] = TILE;
	tp.sourcePaths[ParentExt] = (std::make_pair(parent, parent));
	tp.sourceSCpathLengths[ParentExt] = 0;

	//	if(parent->getIdx() == 22){
	//		LLVM_DEBUG(dbgs() << "createTreePath::Parent=" << parent->getIdx() << "\n";
	//		LLVM_DEBUG(dbgs() << "createTreePath::ParentExt=" << ParentExt->getName() << "\n";
	//	}

	assert(parent != NULL);

	tp.dest = dest;
	bool foundParentTreeBasedRoutingLocs = false;

	for (int i = 0; i < parent->getChildren().size(); ++i)
	{
		child = parent->getChildren()[i];
		if (child->getMappedLoc() != NULL)
		{
			foundParentTreeBasedRoutingLocs = false;
			if ((*child->getTreeBasedRoutingLocs()).find(parent) != (*child->getTreeBasedRoutingLocs()).end())
			{
				for (int j = 0; j < (*child->getTreeBasedRoutingLocs())[parent].size(); ++j)
				{
					cnode = (*child->getTreeBasedRoutingLocs())[parent][j]->cnode;
					tp.sources.push_back(cnode);
					tp.sourcePorts[cnode] = (*child->getTreeBasedRoutingLocs())[parent][j]->lastPort;

					//					LLVM_DEBUG(dbgs() << "getTreeBasedRoutingLocs read :: ";
					//					LLVM_DEBUG(dbgs() << ", currNode=" << child->getIdx();
					//					LLVM_DEBUG(dbgs() << ", currParent=" << parent->getIdx();
					//					LLVM_DEBUG(dbgs() << ", cnode=" << cnode->getName();
					//					LLVM_DEBUG(dbgs() << ", port=" << getCGRA()->getPortName(tp.sourcePorts[cnode]) << "\n";
					//					LLVM_DEBUG(dbgs() << ", pathLength=" << (*child->getTreeBasedRoutingLocs())[parent][j]->SCpathLength << "\n";
					assert(std::string("INV").compare(getCGRA()->getPortName(tp.sourcePorts[cnode])) != 0);

					tp.sourceSCpathLengths[cnode] = (*child->getTreeBasedRoutingLocs())[parent][j]->SCpathLength;
					tp.sourceSCpathTdimLengths[cnode] = (*child->getTreeBasedRoutingLocs())[parent][j]->TdimPathLength;
					if (cnode == ParentExt)
					{
						tp.sourcePaths[cnode] = (std::make_pair(parent, parent));
					}
					else
					{
						tp.sourcePaths[cnode] = (std::make_pair(parent, child));
					}
					foundParentTreeBasedRoutingLocs = true;
				}
			}

			if ((*child->getTreeBasedGoalLocs()).find(parent) != (*child->getTreeBasedGoalLocs()).end())
			{
				for (int j = 0; j < (*child->getTreeBasedGoalLocs())[parent].size(); ++j)
				{
					cnode = (*child->getTreeBasedGoalLocs())[parent][j]->cnode;
					tp.sources.push_back(cnode);
					tp.sourcePorts[cnode] = (*child->getTreeBasedGoalLocs())[parent][j]->lastPort;
					assert(std::string("INV").compare(getCGRA()->getPortName(tp.sourcePorts[cnode])) != 0);
					tp.sourceSCpathLengths[cnode] = (*child->getTreeBasedGoalLocs())[parent][j]->SCpathLength;
					tp.sourceSCpathTdimLengths[cnode] = (*child->getTreeBasedGoalLocs())[parent][j]->TdimPathLength;
					if (cnode == ParentExt)
					{
						tp.sourcePaths[cnode] = (std::make_pair(parent, parent));
					}
					else
					{
						tp.sourcePaths[cnode] = (std::make_pair(parent, child));
					}
					foundParentTreeBasedRoutingLocs = true;
				}
			}

			if (!foundParentTreeBasedRoutingLocs)
			{
				LLVM_DEBUG(dbgs() << "foundParentTreeBasedRoutingLocs is false!\n");
				LLVM_DEBUG(dbgs() << "child=" << child->getIdx() << "\n");
				LLVM_DEBUG(dbgs() << "childLoc=" << child->getMappedLoc()->getName() << "\n");
				LLVM_DEBUG(dbgs() << "parent=" << parent->getIdx() << "\n");
			}

			assert(tp.sourcePaths[cnode].first != NULL);
			assert(tp.sourcePaths[cnode].second != NULL);
		}
	}

	return tp;
}

//int DFG::findUtilTreeRoutingLocs(CGRANode* cnode, dfgNode* currNode) {
//	dfgNode* node;
//	std::map<dfgNode*,std::vector<std::pair<CGRANode*,Port> >>::iterator treeRoutingLocsIt;
//	std::vector<std::pair<CGRANode*,Port> > cnodes;
//	int util = 0;
//
//	for (int i = 0; i < NodeList.size(); ++i) {
//		node = NodeList[i];
//
//		if( (node->getMappedLoc() != NULL) || (node == currNode)/*&&(node->getASAPnumber() <= currNode->getASAPnumber())*/){
//
//			if(node->getMappedLoc() == cnode){
//				continue;
//			}
//
//			for(treeRoutingLocsIt = node->getTreeBasedRoutingLocs()->begin();
//				treeRoutingLocsIt != node->getTreeBasedRoutingLocs()->end();
//				treeRoutingLocsIt++){
//
//				cnodes = treeRoutingLocsIt->second;
//				if(std::find(cnodes.begin(),cnodes.end(),cnode) != cnodes.end()){
//					util++;
//				}
//
//
//			}
//		}
//	}
//	return util;
//}

void DFG::printOutSMARTRoutes()
{
	dfgNode *node;
	dfgNode *parent;
	dfgNode *origNode;
	dfgNode *origParent;

	std::vector<std::ofstream> outFiles(currCGRA->getYdim() * currCGRA->getXdim());
	std::ofstream *currOutFile;
	CGRANode *cnode;
	CGRANode *routingCnode;
	CGRANode *parentExt;
	std::map<int, std::vector<std::string>> logEntries;
	bool noRouting = true;
	int MII = currCGRA->getMII();
	std::pair<dfgNode *, dfgNode *> sourcePath;

	int pT = -1;
	int pY = -1;
	int pX = -1;

	//Sanity check
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getMappedLoc() == NULL)
		{
			LLVM_DEBUG(dbgs() << "printOutSMARTRoutes :: All the nodes are not mapped!\n");
			return;
		}
	}

	for (int y = 0; y < currCGRA->getYdim(); ++y)
	{
		for (int x = 0; x < currCGRA->getXdim(); ++x)
		{
			std::string tempOutFileName = name + "_SMRT_" + std::to_string(y) + "_" + std::to_string(x) + ".log";
			outFiles[convertToPhyLoc(y, x)].open(tempOutFileName.c_str());
		}
	}

	int routeStart = 0;
	int k;
	//Generate Log Entries
	for (int t = 0; t < currCGRA->getMII(); ++t)
	{
		for (int y = 0; y < currCGRA->getYdim(); ++y)
		{
			for (int x = 0; x < currCGRA->getXdim(); ++x)
			{
				cnode = currCGRA->getCGRANode(t, y, x);
				if (cnode->getmappedDFGNode() != NULL)
				{
					node = cnode->getmappedDFGNode();
					origNode = node;
					if (node->getMappedLoc() == cnode)
					{ // node is not just a routing location
						for (int i = 0; i < node->getAncestors().size(); ++i)
						{
							parent = node->getAncestors()[i];
							origParent = parent;

							if (parent->getMappedLoc()->getPEType() == MEM)
							{
								parentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 2) % MII, parent->getMappedLoc()->getY(), parent->getMappedLoc()->getX());
							}
							else
							{
								parentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 1) % MII, parent->getMappedLoc()->getY(), parent->getMappedLoc()->getX());
							}

							routeStart = 0;
							std::string strEntry;
							routingCnode = node->getMappedLoc();
							LLVM_DEBUG(dbgs() << "ParentExt=" << parentExt->getName() << "\n");
							LLVM_DEBUG(dbgs() << "FinalDest=" << routingCnode->getName() << "\n");
							LLVM_DEBUG(dbgs() << "FinalDestNodeIdx=" << node->getIdx() << "\n");
							LLVM_DEBUG(dbgs() << "FinalParentNodeIdx=" << parent->getIdx() << "\n");
							LLVM_DEBUG(dbgs() << "ParentsSize=" << node->getAncestors().size() << "\n");
							do
							{
								assert(parent->getMappedLoc() != NULL);
								assert(node->getMappedLoc() != NULL);
								//							strEntry = node->getMappedLoc()->getNameWithOutTime();
								//							routingCnode = node->getMappedLoc();
								//							noRouting = true;
								for (int j = routeStart; j < node->getMergeRoutingLocs()[parent].size(); ++j)
								{
									LLVM_DEBUG(dbgs() << "routePath = " << node->getMergeRoutingLocs()[parent][j]->cnode->getName() << "\n");
									//									if(routingCnode->getT() != node->getMergeRoutingLocs()[parent][j]->getT()){
									//										if(!noRouting){
									//											pT = routingCnode->getT();
									//											pY = routingCnode->getY();
									//											pX = routingCnode->getX();
									//
									//											logEntries[convertToPhyLoc(pT,pY,pX)].push_back(strEntry);
									//										}
									//										routingCnode = node->getMergeRoutingLocs()[parent][j];
									//										noRouting = true;
									//										strEntry =  routingCnode->getNameWithOutTime() + " <-- " ;
									//									}else{
									routingCnode = node->getMergeRoutingLocs()[parent][j]->cnode;
									//										noRouting = false;
									strEntry = strEntry + routingCnode->getName() + " <-- ";
									nodeRouteMap[origNode][origParent].push_back(routingCnode);
									//									}
								}

								//								if(!noRouting){
								//									pT = routingCnode->getT();
								//									pY = routingCnode->getY();
								//									pX = routingCnode->getX();
								//
								//									logEntries[convertToPhyLoc(pT,pY,pX)].push_back(strEntry);
								//								}

								sourcePath = (*node->getSourceRoutingPath())[parent];
								node = sourcePath.second;
								parent = sourcePath.first;
								//								parentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT()+1)%MII,parent->getMappedLoc()->getY(),parent->getMappedLoc()->getX());

								if (node != parent)
								{
									for (k = 0; k < node->getMergeRoutingLocs()[parent].size(); ++k)
									{
										LLVM_DEBUG(dbgs() << "pathNode =" << node->getMergeRoutingLocs()[parent][k]->cnode->getName() << "\n");
										if (node->getMergeRoutingLocs()[parent][k]->cnode == routingCnode)
										{
											routeStart = k + 1;
											break;
										}
									}
									LLVM_DEBUG(dbgs() << "routingCnode = " << routingCnode->getName() << "\n");
									LLVM_DEBUG(dbgs() << "sourcePathNodeIdx = " << node->getIdx() << "\n");
									LLVM_DEBUG(dbgs() << "sourcePathParentIdx = " << parent->getIdx() << "\n");
									LLVM_DEBUG(dbgs() << "sourcePathParentExtLoc = " << parentExt->getName() << "\n");
									LLVM_DEBUG(dbgs() << "sourcePathNodeLoc = " << node->getMappedLoc()->getName() << "\n");
									LLVM_DEBUG(dbgs() << "sourcePathParentLoc = " << parent->getMappedLoc()->getName() << "\n");

									LLVM_DEBUG(dbgs() << "node->getMergeRoutingLocs()[parent].size() = " << node->getMergeRoutingLocs()[parent].size() << "\n");

									assert(routeStart == k + 1);
								}

							} while (node != parent);
							assert(routingCnode == parentExt);
							LLVM_DEBUG(dbgs() << "%% Path Mapping Done ! \n");

							//							if(!noRouting){
							pT = cnode->getT();
							pY = cnode->getY();
							pX = cnode->getX();

							logEntries[convertToPhyLoc(pT, pY, pX)].push_back(strEntry);
							//							}

							node = cnode->getmappedDFGNode();
						}
					}
					else
					{ // node is just a routing location, this will not happen in SMART based routing
						logEntries[convertToPhyLoc(t, y, x)].push_back("ROUTING ONLY");
					}
				}
				else
				{
					logEntries[convertToPhyLoc(t, y, x)].push_back("NIL");
				}
			}
		}
	}

	//Writeout Log Entries
	for (int t = 0; t < currCGRA->getMII(); ++t)
	{
		for (int y = 0; y < currCGRA->getYdim(); ++y)
		{
			for (int x = 0; x < currCGRA->getXdim(); ++x)
			{
				for (int i = 0; i < logEntries[convertToPhyLoc(t, y, x)].size(); ++i)
				{
					outFiles[convertToPhyLoc(y, x)] << std::to_string(t) << "," << logEntries[convertToPhyLoc(t, y, x)][i] << std::endl;
				}
			}
		}
	}

	for (int i = 0; i < outFiles.size(); ++i)
	{
		outFiles[i].close();
	}

	std::ofstream pathLengthFile;
	std::string pathLengthFileName = name + "_routelengthstats.csv";
	pathLengthFile.open(pathLengthFileName.c_str());

	//Print Header
	pathLengthFile << "Node,Parent,Route,RouteLength,dT(RouteLength)/MII" << std::endl;

	std::map<dfgNode *, std::map<dfgNode *, std::vector<CGRANode *>>>::iterator nodeIt;
	std::map<dfgNode *, std::vector<CGRANode *>>::iterator ParentIt;

	for (nodeIt = nodeRouteMap.begin();
			nodeIt != nodeRouteMap.end();
			nodeIt++)
	{

		node = nodeIt->first;
		for (ParentIt = nodeRouteMap[node].begin();
				ParentIt != nodeRouteMap[node].end();
				ParentIt++)
		{

			parent = ParentIt->first;

			pathLengthFile << std::to_string(node->getIdx()) << ",";
			pathLengthFile << std::to_string(parent->getIdx()) << ",";

			assert(!nodeRouteMap[node][parent].empty());
			int endTime = nodeRouteMap[node][parent][0]->getT();
			int revisits = 0;

			for (int i = 0; i < nodeRouteMap[node][parent].size(); ++i)
			{
				pathLengthFile << nodeRouteMap[node][parent][i]->getNameSp() << "<--";

				if (i != 0)
				{
					if (nodeRouteMap[node][parent][i]->getT() == endTime)
					{
						if (nodeRouteMap[node][parent][i - 1]->getT() != nodeRouteMap[node][parent][i]->getT())
						{
							revisits++;
						}
					}
				}
			}
			pathLengthFile << ",";

			pathLengthFile << nodeRouteMap[node][parent].size() << ",";
			pathLengthFile << revisits << std::endl;
			//			pathLengthFile << nodeRouteMap[node][parent].size()/currCGRA->getMII() << std::endl;
		}
		pathLengthFile << std::endl;
	}
	pathLengthFile.close();
	analyzeRTpaths();
}

int DFG::convertToPhyLoc(int t, int y, int x)
{
	return t * currCGRA->getYdim() * currCGRA->getXdim() + y * currCGRA->getXdim() + x;
}

int DFG::convertToPhyLoc(int y, int x)
{
	return y * currCGRA->getXdim() + x;
}

//This function should be called on the incremented pointers, i.e. Nodes upto index are mapped
//This function should reorder nodeDestMap, i.e. nodes that are not yet mapped considering the tree based routing
bool DFG::EMSSortNodeDest(
		std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> *nodeDestMap,
		std::map<CGRANode *, std::vector<CGRAEdge>> cgraEdges,
		int index)
{

	//Nodes upto index are mapped
	std::vector<nodeWithCost>::iterator it = globalNodesWithCost.begin() + index;
	std::vector<nodeWithCost>::iterator localtItPrev;
	std::vector<nodeWithCost>::iterator localtItForward;
	dfgNode *node = it->node;
	int affCost = -1;
	int routeCost = -1;
	std::vector<nodeWithCost> localNodesWithCost;
	std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> newNodeDestMap;
	CGRANode *cnode;
	int mappedRealTime = -1;
	bool result = true;

	//Loop through mapped nodes
	for (localtItPrev = it - 1; localtItPrev != it; localtItPrev++)
	{
		//Assumption : All nodes are mapped until this point
		assert(localtItPrev->node->getMappedLoc() != NULL);
		LLVM_DEBUG(dbgs() << "localtItPrev, NodeIdx = " << localtItPrev->node->getIdx() << "\n");

		for (localtItForward = it; localtItForward != globalNodesWithCost.end(); localtItForward++)
		{
			LLVM_DEBUG(dbgs() << "localtItForward, NodeIdx = " << localtItForward->node->getIdx() << "\n");
			if (localtItForward->node->getASAPnumber() == 16)
			{
				LLVM_DEBUG(dbgs() << "getAffinityCost started.\n");
			}
			affCost = getAffinityCost(localtItForward->node, localtItPrev->node);
			//TODO :removing affcost for now, add it later
			//			affCost = 0;
			if (localtItForward->node->getASAPnumber() == 16)
			{
				LLVM_DEBUG(dbgs() << "getAffinityCost done.\n");
				LLVM_DEBUG(dbgs() << "(*nodeDestMap)[localtItForward->node].size = " << (*nodeDestMap)[localtItForward->node].size() << "\n");
			}

			localNodesWithCost.clear();
			for (int j = 0; j < (*nodeDestMap)[localtItForward->node].size(); ++j)
			{
				if (localtItForward->node->getASAPnumber() == 16)
				{
					LLVM_DEBUG(dbgs() << "(*nodeDestMap)[localtItForward->node], j = " << j << "\n");
				}

				cnode = (*nodeDestMap)[localtItForward->node][j].first;
				mappedRealTime = (*nodeDestMap)[localtItForward->node][j].second;

				routeCost = getStaticRoutingCost(localtItForward->node, cnode, cgraEdges);
				if (routeCost == INT_MAX)
				{   //Not Routed
					//					localNodesWithCost.push_back(nodeWithCost(localtItForward->node,cnode,INT_MAX,mappedRealTime));
					//					localNodesWithCost[localNodesWithCost.size()-1].affinityCost = 0;
					//					localNodesWithCost[localNodesWithCost.size()-1].routingCost = routeCost;
				}
				else
				{
					affCost = affCost * getDistCGRANodes(cnode, localtItPrev->node->getMappedLoc());
					localNodesWithCost.push_back(nodeWithCost(localtItForward->node, cnode, routeCost + affCost, mappedRealTime));
					localNodesWithCost[localNodesWithCost.size() - 1].affinityCost = affCost;
					localNodesWithCost[localNodesWithCost.size() - 1].routingCost = routeCost;
				}
			}
			//			assert(localNodesWithCost.size() == (*nodeDestMap)[localtItForward->node].size());
			if (localNodesWithCost.size() == 0)
			{
				result = false;
			}
			else
			{
				std::sort(localNodesWithCost.begin(), localNodesWithCost.end(), LessThanNodeWithCost());
				(*nodeDestMap)[localtItForward->node].clear();
				for (int j = 0; j < localNodesWithCost.size(); ++j)
				{
					(*nodeDestMap)[localtItForward->node].push_back(std::make_pair(localNodesWithCost[j].cnode, localNodesWithCost[j].mappedRealTime));
				}
				assert(localNodesWithCost.size() != 0);
				localtItForward->cost = localNodesWithCost[0].cost;
				LLVM_DEBUG(dbgs() << "routeCost=" << localNodesWithCost[0].routingCost << ", affCost=" << localNodesWithCost[0].affinityCost << ", Total=" << localNodesWithCost[0].cost << "\n");
			}
		}
	}
	std::sort(it, globalNodesWithCost.end(), LessThanNodeWithCost());
	return result;
}

int DFG::getDistCGRANodes(CGRANode *a, CGRANode *b)
{
	int aX = a->getX();
	int aY = a->getY();
	int aT = a->getT();

	int bX = b->getX();
	int bY = b->getY();
	int bT = b->getT();

	return abs(aX - bX) + abs(aY - bY) + abs(aT - bT);
}

void DFG::printConMat(std::vector<std::vector<unsigned char>> conMat)
{
	LLVM_DEBUG(dbgs() << "Printing ConMat....\n");
	for (int i = 0; i < conMat.size(); ++i)
	{
		for (int j = 0; j < conMat[i].size(); ++j)
		{
			LLVM_DEBUG(dbgs() << (int)conMat[i][j] << " ");
		}
		LLVM_DEBUG(dbgs() << "\n");
	}
	LLVM_DEBUG(dbgs() << "Done Printing ConMat....\n");
}

int DFG::getStaticRoutingCost(dfgNode *node, CGRANode *dest, std::map<CGRANode *, std::vector<CGRAEdge>> Edges)
{
	dfgNode *parent;
	TreePath tp;
	CGRANode *end;
	Port endPort;

	CGRANode *cnode;
	Port currPort;
	std::pair<CGRANode *, Port> currNodePortPair;
	int currCost = 0;
	int bestCost = INT_MAX;
	CGRANode *bestSource = NULL;

	int cost = 0;

	std::map<std::pair<CGRANode *, Port>, std::pair<CGRANode *, Port>> cameFrom;
	std::map<std::pair<CGRANode *, Port>, std::pair<CGRANode *, Port>>::iterator cameFromIt;
	std::map<CGRANode *, int> costSoFar;

	std::vector<CGRAEdge *> tempCGRAEdges;

	for (int i = 0; i < node->getAncestors().size(); ++i)
	{
		parent = node->getAncestors()[i];
		tp = createTreePath(parent, dest);

		bestSource = NULL;
		for (int j = 0; j < tp.sources.size(); ++j)
		{
			end = astar->AStarSearchEMS(Edges, tp.sources[j], dest, &cameFrom, &costSoFar, &endPort);
			if (end != dest)
			{
				continue;
			}

			//traverse the path
			cnode = dest;
			for (cameFromIt = cameFrom.begin(); cameFromIt != cameFrom.end(); cameFromIt++)
			{
				if (cameFromIt->first.first == cnode)
				{
					currPort = cameFromIt->first.second;
					break;
				}
			}
			currNodePortPair = std::make_pair(cnode, currPort);

			currCost = 0;
			while (cnode != tp.sources[j])
			{
				if ((cnode->getX() == cameFrom[currNodePortPair].first->getX()) &&
						(cnode->getY() == cameFrom[currNodePortPair].first->getY()))
				{
					currCost++;
				}
				else
				{
					if (cnode == dest)
					{
					}
					else
					{
						currCost += currCGRA->getMII();
					}
				}
				//				currCost += (currCGRA->getCGRANode(0,1,1)->originalEdgesSize - (*currCGRA->getCGRAEdges())[cnode].size());
				cnode = cameFrom[currNodePortPair].first;
			}

			if (currCost < bestCost)
			{
				bestCost = currCost;
				bestSource = tp.sources[j];
			}
		}

		if (bestSource == NULL)
		{
			//			LLVM_DEBUG(dbgs() << "getStaticRoutingCost :: routing ParentIdx=" << parent->getIdx();
			//			LLVM_DEBUG(dbgs() << ", placed=" << parent->getMappedLoc()->getName();
			//			LLVM_DEBUG(dbgs() << " to nodeIdx" << node->getIdx() << ", triedToBePlaced=" << dest->getName();
			//			LLVM_DEBUG(dbgs() << " FAILED! \n";
			return INT_MAX;
		}
		cost = cost + bestCost;
	}
	return cost;
}
