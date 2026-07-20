#include "DfgCoreInternal.h"

std::vector<std::vector<unsigned char>> DFG::selfMulConMat(
		std::vector<std::vector<unsigned char>> in)
{

	std::vector<std::vector<unsigned char>> out;

	for (int i = 0; i < in.size(); ++i)
	{

		//Only support square matrices
		assert(in.size() == in[i].size());

		std::vector<unsigned char> tempVec;
		for (int j = 0; j < in.size(); ++j)
		{
			unsigned char tempInt = 0;
			for (int k = 0; k < in.size(); ++k)
			{
				tempInt = tempInt | (in[i][k] & in[k][j]);
			}
			tempVec.push_back(tempInt);
		}
		out.push_back(tempVec);
	}

	return out;
}

std::vector<std::vector<unsigned char>> DFG::getConMat()
{

	int nodelistSize = NodeList.size();

	std::vector<std::vector<unsigned char>> conMat;
	dfgNode *node;
	dfgNode *child;

	for (int i = 0; i < nodelistSize; ++i)
	{
		std::vector<unsigned char> temp;
		for (int j = 0; j < nodelistSize; ++j)
		{
			temp.push_back(0);
		}
		conMat.push_back(temp);
	}

	for (int i = 0; i < nodelistSize; ++i)
	{
		node = NodeList[i];
		for (int j = 0; j < node->getChildren().size(); ++j)
		{
			child = node->getChildren()[j];
			conMat[node->getIdx()][child->getIdx()] = 1;
		}
	}

	return conMat;
}

int DFG::getAffinityCost(dfgNode *a, dfgNode *b)
{
	int nodelistSize = NodeList.size();

	assert(a->getASAPnumber() == b->getASAPnumber());
	int currLevel = a->getASAPnumber();
	int maxDist = std::min(maxASAPLevel - currLevel, 2);

	int affLevel = 0;
	int affCost = 0;

	if (maxASAPLevel == currLevel)
	{
		return 0;
	}

	while (1)
	{
		for (int i = 0; i < nodelistSize; ++i)
		{

			if (conMatArr[affLevel][a->getIdx()][i] == 1)
			{
				if (conMatArr[affLevel][b->getIdx()][i] == 1)
				{
					//					affCost = affCost + 2**(maxDist - (affLevel+1));
					//					affCost = affCost + (int)pow(2,(maxDist - (affLevel+1)));
					affCost = affCost + (2 << (maxDist - (affLevel + 1)));
				}
			}
		}

		affLevel++;
		if (affLevel == maxDist)
		{
			return affCost;
		}

		if (affLevel == conMatArr.size())
		{
			conMatArr.push_back(selfMulConMat(conMatArr[affLevel - 1]));
		}
	}
}

std::vector<int> DFG::getIntersection(std::vector<std::vector<int>> vectors)
{

	assert(vectors.size() != 0);
	std::vector<int> result;

	for (int i = 0; i < vectors[0].size(); ++i)
	{
		result.push_back(1);
	}

	for (int i = 1; i < vectors.size(); ++i)
	{
		assert(vectors[i].size() == vectors[0].size());
		for (int j = 0; j < vectors[i].size(); ++j)
		{
			result[j] = result[j] & vectors[i][j];
		}
	}

	return result;
}

int DFG::getDist(CGRANode *src, CGRANode *dest)
{
	assert(src->getmappedDFGNode() != NULL);
	assert(src->getmappedDFGNode() != NULL);

	int xCost = abs(src->getX() - dest->getX());
	int yCost = abs(src->getY() - dest->getY());
	int tCost = dest->getT() - src->getT();

	if (tCost < 0)
	{
		return INT32_MAX;
	}

	if (xCost + yCost > tCost)
	{
		return INT32_MAX;
	}

	return xCost + yCost;
}

int DFG::AStarSP(CGRANode *src, CGRANode *dest, std::vector<CGRANode *> *path)
{

	struct LessThanFScore
	{
		bool operator()(const std::pair<CGRANode *, int> &left, const std::pair<CGRANode *, int> &right) const
		{
			return left.second < right.second;
		}
	};

	path->clear();
	std::vector<ConnectedCGRANode> connectedNodes;
	std::vector<CGRANode *> closedSet;
	std::vector<CGRANode *> openSet;

	std::map<CGRANode *, int> fscore;
	std::map<CGRANode *, int> gscore;
	std::map<CGRANode *, CGRANode *> prevNode;
	//	std::priority_queue<AStarNode, std::vector<AStarNode>, LessThanCost> openSet;

	openSet.push_back(src);
	fscore[src] = getDist(src, dest);

	if (fscore[src] == INT32_MAX)
	{
		return -1;
	}

	gscore[src] = 0;

	CGRANode *current;
	int tentgscore;
	int dist;
	while (!openSet.empty())
	{
		//		anode = openSet.top();
		std::map<CGRANode *, int>::iterator it = std::min_element(fscore.begin(), fscore.end(), LessThanFScore());
		current = (*it).first;
		fscore.erase(it);
		openSet.erase(std::remove(openSet.begin(), openSet.end(), current), openSet.end());
		closedSet.push_back(current);
		//		openSet.pop();

		if (current == dest)
		{
			break;
		}

		connectedNodes = current->getConnectedNodes();
		for (int i = 0; i < connectedNodes.size(); ++i)
		{

			if (connectedNodes[i].node->getmappedDFGNode() != NULL)
			{
				continue;
			}

			dist = getDist(current, connectedNodes[i].node);
			if (dist == INT32_MAX || gscore[current] == INT32_MAX)
			{
				tentgscore = INT32_MAX;
			}
			else
			{
				tentgscore = gscore[current] + getDist(current, connectedNodes[i].node);
			}

			if (std::find(closedSet.begin(), closedSet.end(), connectedNodes[i].node) != closedSet.end())
			{
				continue;
			}

			if (std::find(openSet.begin(), openSet.end(), connectedNodes[i].node) == openSet.end())
			{
				openSet.push_back(connectedNodes[i].node);
			}
			else
			{
				if (gscore.find(connectedNodes[i].node) == gscore.end())
				{
					gscore[connectedNodes[i].node] = INT32_MAX;
				}

				if (tentgscore >= gscore[connectedNodes[i].node])
				{
					continue;
				}
			}

			prevNode[connectedNodes[i].node] = current;
			gscore[connectedNodes[i].node] = tentgscore;

			dist = getDist(connectedNodes[i].node, dest);
			if (dist == INT32_MAX)
			{
				fscore[connectedNodes[i].node] = INT32_MAX;
			}
			else
			{
				fscore[connectedNodes[i].node] = tentgscore + getDist(connectedNodes[i].node, dest);
			}
		}
	}

	int routeCost = 0;

	if (current != dest)
	{
		return -1;
	}
	else
	{
		while (current != src)
		{
			path->push_back(current);
			current = prevNode[current];
			routeCost++;
		}
		return routeCost;
	}
}

int DFG::AddRoutingEdges(dfgNode *node)
{

	std::vector<dfgNode *> parents;
	CGRANode *cnode;
	std::vector<std::vector<int>> parentConnectedPhyNodes;
	std::vector<int> parentIntersectionTmp;
	int routingPossibilities = 0;

	for (int j = 0; j < node->getAncestors().size(); ++j)
	{
		parents.push_back(node->getAncestors()[j]);
	}

	for (int j = 0; j < parents.size(); ++j)
	{
		parentConnectedPhyNodes.push_back(currCGRA->getPhyConMatNode(parents[j]->getMappedLoc()));
	}

	do
	{
		parentIntersectionTmp = getIntersection(parentConnectedPhyNodes);

		routingPossibilities = 0;
		for (int j = 0; j < parentIntersectionTmp.size(); ++j)
		{
			cnode = currCGRA->getCGRANode(j);
			if (cnode->getmappedDFGNode() != NULL)
			{
				parentIntersectionTmp[j] = 0;
			}

			routingPossibilities = routingPossibilities + parentIntersectionTmp[j];
		}

		if (routingPossibilities == 0)
		{
		}

	} while (routingPossibilities == 0);

	return 0;
}

std::map<dfgNode *, std::vector<CGRANode *>> DFG::getPrimarySlots(
		std::vector<dfgNode *> nodes)
{

	dfgNode *node;
	//	dfgNode* parent;
	CGRANode *cnode;

	std::map<dfgNode *, std::vector<CGRANode *>> result;
	std::vector<dfgNode *> parents;

	std::vector<std::vector<int>> parentConnectedPhyNodes;
	std::vector<int> parentIntersection;
	std::vector<int> parentIntersectionTmp;

	std::vector<std::vector<int>> phyConMat;

	bool commonConsumer = false;
	float probCost;
	int routingPossibilities = 0;

	for (int i = 0; i < nodes.size(); ++i)
	{
		node = nodes[i];
		for (int j = 0; j < node->getAncestors().size(); ++j)
		{
			parents.push_back(node->getAncestors()[j]);
		}

		for (int j = 0; j < parents.size(); ++j)
		{
			parentConnectedPhyNodes.push_back(currCGRA->getPhyConMatNode(parents[j]->getMappedLoc()));
		}

		parentIntersectionTmp = getIntersection(parentConnectedPhyNodes);

		for (int j = 0; j < parentIntersectionTmp.size(); ++j)
		{
			cnode = currCGRA->getCGRANode(j);
			if (cnode->getmappedDFGNode() != NULL)
			{
				parentIntersectionTmp[j] = 0;
			}

			routingPossibilities = routingPossibilities + parentIntersectionTmp[j];
		}

		if (routingPossibilities == 0)
		{
		}

		node->setRoutingPossibilities(routingPossibilities);

		for (int j = 0; j < parentIntersectionTmp.size(); ++j)
		{
			if (parentIntersectionTmp[j] == 1)
			{
				cnode = currCGRA->getCGRANode(j);
				if (cnode->getRoutingNode() != NULL)
				{
					if (node->getRoutingPossibilities() < cnode->getRoutingNode()->getRoutingPossibilities())
					{
						cnode->getRoutingNode()->setRoutingPossibilities(cnode->getRoutingNode()->getRoutingPossibilities() - 1);
						result[cnode->getRoutingNode()].erase(std::remove(result[cnode->getRoutingNode()].begin(), result[cnode->getRoutingNode()].end(), cnode), result[cnode->getRoutingNode()].end());
						cnode->setRoutingNode(node);
					}
				}
				else
				{
					cnode->setRoutingNode(node);
					result[node].push_back(cnode);
				}
			}
		}
	}

	for (int i = 0; i < nodes.size(); ++i)
	{
		node = nodes[i];
		for (int j = 0; j < node->getAncestors().size(); ++j)
		{
			parents.push_back(node->getAncestors()[j]);
		}

		//		if(parents.size() == 0){
		//			for (int var = 0; var < max; ++var) {
		//
		//			}
		//		}

		for (int j = 0; j < parents.size(); ++j)
		{
			parentConnectedPhyNodes.push_back(currCGRA->getPhyConMatNode(parents[j]->getMappedLoc()));
		}

		parentIntersection = getIntersection(parentConnectedPhyNodes);

		while (parentIntersection.size() == 0)
		{
			parentConnectedPhyNodes.clear();
			phyConMat = currCGRA->getPhyConMat();
			//			phyConMat = selfMulConMat(phyConMat);

			for (int j = 0; j < parents.size(); ++j)
			{
				cnode = parents[j]->getMappedLoc();
				parentConnectedPhyNodes.push_back(phyConMat[currCGRA->getConMatIdx(cnode->getT(), cnode->getY(), cnode->getX())]);
			}
		}

		std::vector<CGRANode *> resultNodeArr;
		for (int j = 0; j < parentIntersection.size(); ++j)
		{
			if (parentIntersection[j] == 1)
			{
				resultNodeArr.push_back(currCGRA->getCGRANode(j));
			}
		}
		result[node] = resultNodeArr;
	}

	return result;
}

bool DFG::MapMultiDestRec(
		std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> *nodeDestMap,
		std::map<CGRANode *, std::vector<dfgNode *>> *destNodeMap,
		std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>>::iterator it,
		std::map<CGRANode *, std::vector<CGRAEdge>> cgraEdges,
		int index)
{

	std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> localNodeDestMap = *nodeDestMap;
	std::map<CGRANode *, std::vector<dfgNode *>> localdestNodeMap = *destNodeMap;

	dfgNode *node;
	dfgNode *otherNode;
	dfgNode *parent;
	CGRANode *cnode;
	CGRANode *chosenCnode = NULL;
	std::pair<CGRANode *, int> cnodePair;
	CGRANode *parentExt;
	//	std::vector< std::pair<CGRANode*,int> > possibleDests;
	std::vector<dfgNode *> parents;

	//	std::vector<std::pair<CGRANode*, CGRANode*> > paths;
	std::vector<CGRANode *> dests;
	std::map<CGRANode *, int> destllMap;

	std::vector<std::pair<CGRANode *, CGRANode *>> pathsNotRouted;
	bool success = false;

	//	CGRA localCGRA = *inCGRA;
	std::map<CGRANode *, std::vector<CGRAEdge>> localCGRAEdges = cgraEdges;

	node = it->first;
	//	possibleDests = it->second;

	bool routeComplete = false;

	std::vector<std::pair<CGRANode *, int>> PossibleDests = it->second;

	LLVM_DEBUG(dbgs() << "MapMultiDestRec : Procesing NodeIdx = " << node->getIdx());
	LLVM_DEBUG(dbgs() << ", PossibleDests = " << it->second.size());
	LLVM_DEBUG(dbgs() << ", MII = " << currCGRA->getMII());
	LLVM_DEBUG(dbgs() << ", currASAPLevel = " << node->getASAPnumber() << "/" << maxASAPLevel);
	LLVM_DEBUG(dbgs() << ", NodeProgress = " << index + 1 << "/" << nodeDestMap->size());
	LLVM_DEBUG(dbgs() << "\n");

	for (int j = 0; j < node->getAncestors().size(); ++j)
	{
		parent = node->getAncestors()[j];
		parentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 1) % (currCGRA->getMII()), parent->getMappedLoc()->getY(), parent->getMappedLoc()->getX());

		LLVM_DEBUG(dbgs() << "ParentIdx=" << parent->getIdx());
		LLVM_DEBUG(dbgs() << ",ParentType=" << parent->getNameType());
		LLVM_DEBUG(dbgs() << ",ParentRT=" << parent->getmappedRealTime());
		LLVM_DEBUG(dbgs() << ",Path = "
				<< "(" << parentExt->getT() << ","
				<< parentExt->getY() << ","
				<< parentExt->getX() << ") to ...\n");
		parents.push_back(parent);
	}

	for (int i = 0; i < it->second.size(); ++i)
	{
		success = false;
		if (it->second[i].first->getmappedDFGNode() == NULL)
		{
			//			LLVM_DEBUG(dbgs() << "Possible Dest = "
			//				   << "(" << it->second[i].first->getT() << ","
			//				   	   	  << it->second[i].first->getY() << ","
			//						  << it->second[i].first->getX() << ")\n";

			cnode = it->second[i].first;
			//			cnodePair = it->second[i];
			dests.push_back(cnode);
			destllMap[cnode] = it->second[i].second;
		}
	}

	std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>>::iterator itlocal;

	while (!success)
	{
		localCGRAEdges = cgraEdges;
		routeComplete = astar->Route(node, parents, &dests, &destllMap, &localCGRAEdges, &pathsNotRouted, &chosenCnode, &deadEndReached);

		LLVM_DEBUG(dbgs() << "MapMultiDestRec::routeComplete=" << routeComplete << "\n");
		LLVM_DEBUG(dbgs() << "MapMultiDestRec::deadEndReached=" << deadEndReached << "\n");

		if (deadEndReached || !routeComplete)
		{
			return false;
		}
		assert(chosenCnode != NULL);
		mappingOutFile << "nodeIdx=" << node->getIdx() << ", placed=(" << chosenCnode->getT() << "," << chosenCnode->getY() << "," << chosenCnode->getX() << ")"
				<< "\n";
		mappingOutFile << "routing success, keeping the current edges\n";

		LLVM_DEBUG(dbgs() << "Placed = "
				<< "(" << chosenCnode->getT() << "," << chosenCnode->getY() << "," << chosenCnode->getX() << ")\n");
		node->setMappedLoc(chosenCnode);
		chosenCnode->setMappedDFGNode(node);
		node->setMappedRealTime(destllMap[chosenCnode]);

		itlocal = it;
		itlocal++;

		if (index + 1 < nodeDestMap->size())
		{
			success = MapMultiDestRec(
					&localNodeDestMap,
					&localdestNodeMap,
					itlocal,
					localCGRAEdges,
					index + 1);
		}
		else
		{
			LLVM_DEBUG(dbgs() << "nodeDestMap end reached..\n");
			*nodeDestMap = localNodeDestMap;
			*destNodeMap = localdestNodeMap;
			currCGRA->setCGRAEdges(localCGRAEdges);
			success = true;
		}

		if (!success)
		{
			LLVM_DEBUG(dbgs() << "MapMultiDestRec : fails next possible destination\n");
			node->setMappedLoc(NULL);
			chosenCnode->setMappedDFGNode(NULL);
			mappingOutFile << std::to_string(index + 1) << " :: mapping failed, therefore trying mapping again for index=" << std::to_string(index) << "\n";
			if (backtrackCounter == 0)
			{
				return false;
			}
			backtrackCounter--;
		}
	}

	backtrackCounter = std::min(initBtrack, backtrackCounter + 1);
	return true;

	//	//Old Code From Here
	//
	//	for (int i = 0; i < it->second.size(); ++i) {
	//		success = false;
	//		LLVM_DEBUG(dbgs() << "Possible Dest = "
	//			   << "(" << it->second[i].first->getT() << ","
	//			   	   	  << it->second[i].first->getY() << ","
	//					  << it->second[i].first->getX() << ")\n";
	//
	//		if(it->second[i].first->getmappedDFGNode() == NULL){
	//			LLVM_DEBUG(dbgs() << "Possible Dest is NULL\n";
	//			cnode = it->second[i].first;
	//			cnodePair = it->second[i];
	//			for (int j = 0; j < node->getAncestors().size(); ++j) {
	//				parent = node->getAncestors()[j];
	////				parentExt = currCGRA->getCGRANode(cnode->getT(),parent->getMappedLoc()->getY(),parent->getMappedLoc()->getX());
	////				parentExt = parent->getMappedLoc();
	//				parentExt = currCGRA->getCGRANode((parent->getMappedLoc()->getT() + 1)%(currCGRA->getMII()),parent->getMappedLoc()->getY(),parent->getMappedLoc()->getX());
	//
	//				LLVM_DEBUG(dbgs() << "Path = "
	//					   << "(" << parentExt->getT() << ","
	//					   	   	  << parentExt->getY() << ","
	//							  << parentExt->getX() << ") to ";
	//
	//				LLVM_DEBUG(dbgs() << "(" << cnode->getT() << ","
	//					   	   	  << cnode->getY() << ","
	//							  << cnode->getX() << ")\n";
	//
	////				paths.push_back(std::make_pair(parentExt,cnode));
	////				treePaths.push_back(createTreePath(parent,cnode));
	//				dests.push_back(cnode);
	//				parents.push_back(parent);
	//			}
	//
	//			localCGRAEdges = cgraEdges;
	//			mappingOutFile << "nodeIdx=" << node->getIdx() << ", placed=(" << cnode->getT() << "," << cnode->getY() << "," << cnode->getX() << ")" << "\n";
	////			astar->Route(node,parents,paths,&localCGRAEdges,&pathsNotRouted);
	//			astar->Route(node,parents,dests,&localCGRAEdges,&pathsNotRouted,&deadEndReached);
	//			dests.clear();
	////			paths.clear();
	//			if(!pathsNotRouted.empty()){
	//				mappingOutFile << "routing failed, clearing edges\n";
	//				LLVM_DEBUG(dbgs() << "all paths are not routed.\n";
	//				pathsNotRouted.clear();
	//				if(deadEndReached){
	//					return false;
	//				}
	//				continue;
	//			}
	//			mappingOutFile << "routing success, keeping the current edges\n";
	//			pathsNotRouted.clear();
	//
	//			for (int j = 0; j < localdestNodeMap[cnode].size(); ++j) {
	//				otherNode = localdestNodeMap[cnode][j];
	//				localNodeDestMap[otherNode].erase(std::remove(localNodeDestMap[otherNode].begin(), localNodeDestMap[otherNode].end(), cnodePair), localNodeDestMap[otherNode].end());
	//			}
	//
	////			it->second.clear();
	////			it->second.push_back(cnodePair);
	//			localdestNodeMap[cnode].clear();
	//
	//			LLVM_DEBUG(dbgs() << "Placed = " << "(" << cnode->getT() << "," << cnode->getY() << "," << cnode->getX() << ")\n";
	//			node->setMappedLoc(cnode);
	//			cnode->setMappedDFGNode(node);
	//			node->setMappedRealTime(cnodePair.second);
	//
	//			std::map<dfgNode*,std::vector< std::pair<CGRANode*,int> > >::iterator itlocal;
	//			itlocal = it;
	//			itlocal++;
	////			it++;
	//
	//			if(index + 1 < nodeDestMap->size()){
	//				success = MapMultiDestRec(
	//						&localNodeDestMap,
	//						&localdestNodeMap,
	//						itlocal,
	//						localCGRAEdges,
	//						index + 1);
	//			}
	//			else{
	//				LLVM_DEBUG(dbgs() << "nodeDestMap end reached..\n";
	//				*nodeDestMap = localNodeDestMap;
	//				*destNodeMap = localdestNodeMap;
	//				currCGRA->setCGRAEdges(localCGRAEdges);
	//				success = true;
	//			}
	//
	//			if(deadEndReached){
	//				success = false;
	//				break;
	//			}
	//
	//
	//			if(success){
	//				it->second.clear();
	//				it->second.push_back(cnodePair);
	//				break;
	//			}
	//			else{
	//				LLVM_DEBUG(dbgs() << "MapMultiDestRec : fails next possible destination\n";
	//				node->setMappedLoc(NULL);
	//				cnode->setMappedDFGNode(NULL);
	//				mappingOutFile << std::to_string(index + 1) << " :: mapping failed, therefore trying mapping again for index=" << std::to_string(index) << "\n";
	//			}
	//		}
	//	}
	//
	//	if(success){
	//
	//	}
	//	return success;
}

//bool DFG::MapMultiDest(std::map<dfgNode*, std::vector<CGRANode*> > *nodeDestMap,std::map<CGRANode*, std::vector<dfgNode*> > *destNodeMap) {
//	std::map<dfgNode*, std::vector<CGRANode*> > localNodeDestMap = *nodeDestMap;
//	std::map<CGRANode*, std::vector<dfgNode*> > localdestNodeMap = *destNodeMap;
//
//	dfgNode* node;
//	dfgNode* otherNode;
//	dfgNode* parent;
//	CGRANode* cnode;
//	CGRANode* parentExt;
//	std::vector<CGRANode*> possibleDests;
//	std::vector<std::pair<CGRANode*, CGRANode*> > paths;
//	bool success = false;
//
//	std::map<dfgNode*, std::vector<CGRANode*> > ::iterator it;
////	for (it = localNodeDestMap.begin(); it != localNodeDestMap.end(); it++){
//	it = localNodeDestMap.begin();
//
//
////	}
//
//	*nodeDestMap = localNodeDestMap;
//	*destNodeMap = localdestNodeMap;
//	return true;
//}

bool DFG::MapASAPLevel(int MII, int XDim, int YDim, ArchType arch)
{

	astar = new AStar(&mappingOutFile, MII, this);
	currCGRA = new CGRA(MII, XDim, YDim, REGS_PER_NODE, arch /*RegXbarTREG*/);

	for (int i = 0; i < subLoopDFGs.size(); ++i)
	{
		assert(currCGRA->PlaceMacro(subLoopDFGs[i]));
	}

	currCGRA->setMapped(true);

	LLVM_DEBUG(dbgs() << "STARTING MAPASAP with MII = " << MII << "with maxASAPLevel = " << maxASAPLevel << "\n");

	//	std::map<dfgNode*,std::vector<CGRANode*> > nodeDestMap;
	std::map<dfgNode *, std::vector<std::pair<CGRANode *, int>>> nodeDestMap;
	std::map<CGRANode *, std::vector<dfgNode *>> destNodeMap;
	std::map<dfgNode *, std::map<CGRANode *, int>> nodeDestCostMap;

	std::vector<dfgNode *> currLevelNodes;
	dfgNode *node;
	dfgNode *phiChild;
	dfgNode *parent;
	CGRANode *parentExt;

	for (int level = 0; level <= maxASAPLevel; ++level)
	{
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
				astar->ASAPLevelNodeMap[level].push_back(node);
			}
		}

		LLVM_DEBUG(dbgs() << "numOfNodes = " << currLevelNodes.size() << "\n");

		for (int i = 0; i < currLevelNodes.size(); ++i)
		{
			node = currLevelNodes[i];
			int ll = (node->getASAPnumber() / MII) * MII - 1;
			//				int ll = node->getASAPnumber()-1;
			//				int ll=-1;
			int el = INT32_MAX;
			dfgNode *latestParent;
			for (int j = 0; j < node->getAncestors().size(); ++j)
			{
				parent = node->getAncestors()[j];

				//every parent should be mapped
				if (parent->getMappedLoc() == NULL)
				{
					LLVM_DEBUG(dbgs() << "Parent : " << parent->getIdx() << " is not mapped!, this node=" << node->getIdx() << "\n");
				}
				assert(parent->getMappedLoc() != NULL);

				if (parent->getmappedRealTime() > ll)
				{
					if (parent->getmappedRealTime() % MII != parent->getMappedLoc()->getT())
					{
						LLVM_DEBUG(dbgs() << "parent->getmappedRealTime()%MII = " << parent->getmappedRealTime() % MII << "\n");
						LLVM_DEBUG(dbgs() << "parent->getMappedLoc()->getT() = " << parent->getMappedLoc()->getT() << "\n");
					}

					assert(parent->getmappedRealTime() % MII == parent->getMappedLoc()->getT());
					//						ll = parent->getMappedLoc()->getT();
					ll = parent->getmappedRealTime();
					latestParent = parent;
				}

				//					if(parent->getmappedRealTime() < el){
				//						assert( parent->getmappedRealTime()%MII == parent->getMappedLoc()->getT() );
				//						el = parent->getMappedLoc()->getT();
				////						el = parent->getmappedRealTime();
				//					}
			}
			//				if(!node->getAncestors().empty()){
			//					ll+=node->getASAPnumber() - latestParent->getASAPnumber() - 1;
			//				}
			LLVM_DEBUG(dbgs() << "ll=" << ll << "\n");

			if (!node->getRecAncestors().empty())
			{
				LLVM_DEBUG(dbgs() << "RecAnc for Node" << node->getIdx());
				mappingOutFile << "RecAnc for Node" << node->getIdx();
			}

			for (int j = 0; j < node->getRecAncestors().size(); ++j)
			{
				parent = node->getRecAncestors()[j];

				LLVM_DEBUG(dbgs() << " (Id=" << parent->getIdx() <<

						",rt=" << parent->getmappedRealTime() << ",t=" << parent->getMappedLoc()->getT() << "),");

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

				if (/*!node->getPHIchildren().empty()*/ false)
				{

					assert(node->getPHIchildren().size() == 1);
					phiChild = node->getPHIchildren()[0];
					assert(phiChild->getMappedLoc() != NULL);
					int y = phiChild->getMappedLoc()->getY();
					int x = phiChild->getMappedLoc()->getX();

					if (currCGRA->getCGRANode((ll + 1) % MII, y, x)->getmappedDFGNode() == NULL)
					{
						if (node->getIsMemOp() == (currCGRA->getCGRANode((ll + 1) % MII, y, x)->getPEType() == MEM))
						{

							nodeDestMap[node].push_back(std::make_pair(currCGRA->getCGRANode((ll + 1) % MII, y, x), (ll + 1)));
							destNodeMap[currCGRA->getCGRANode((ll + 1) % MII, y, x)].push_back(node);
						}
					}
				}
				else
				{

					for (int y = 0; y < YDim; ++y)
					{
						//Remove this after running mad benchmark
						if (nodeDestMap[node].size() >= 100)
						{
							break;
						}
						for (int x = 0; x < XDim; ++x)
						{
							//Remove this after running mad benchmark
							if (nodeDestMap[node].size() >= 100)
							{
								break;
							}
							if (currCGRA->getCGRANode((ll + 1) % MII, y, x)->getmappedDFGNode() == NULL)
							{
								if (node->getIsMemOp())
								{ //if current operation is a memory operation
									if (currCGRA->getCGRANode((ll + 1) % MII, y, x)->getPEType() == MEM)
									{ // only allocate PEs capable of doing memory operations

										if (node->getLeftAlignedMemOp() == 1)
										{
											if (true)
											{
												//													if(x==0){
												//													if((y==0||y==1)&&x==0){
												//													if(( (y < currCGRA->getYdim()/2) || (currCGRA->getYdim() == 1) )&&x==0){
												nodeDestMap[node].push_back(std::make_pair(currCGRA->getCGRANode((ll + 1) % MII, y, x), (ll + 1)));
												destNodeMap[currCGRA->getCGRANode((ll + 1) % MII, y, x)].push_back(node);
											}
										}
										else if (node->getLeftAlignedMemOp() == 2)
										{
											if (true)
											{
												//													if(x==currCGRA->getXdim()-1){
												//													assert(currCGRA->getYdim() > 2);
												//													if((y==2||y==3)&&x==0){
												//													if(( (y >= currCGRA->getYdim()/2) || (currCGRA->getYdim() == 1) )&&x==0){
												nodeDestMap[node].push_back(std::make_pair(currCGRA->getCGRANode((ll + 1) % MII, y, x), (ll + 1)));
												destNodeMap[currCGRA->getCGRANode((ll + 1) % MII, y, x)].push_back(node);
											}
										}
										else
										{
											nodeDestMap[node].push_back(std::make_pair(currCGRA->getCGRANode((ll + 1) % MII, y, x), (ll + 1)));
											destNodeMap[currCGRA->getCGRANode((ll + 1) % MII, y, x)].push_back(node);
										}
									}
								}
								else
								{ // for any other operations can allocate any PE.
									nodeDestMap[node].push_back(std::make_pair(currCGRA->getCGRANode((ll + 1) % MII, y, x), (ll + 1)));
									destNodeMap[currCGRA->getCGRANode((ll + 1) % MII, y, x)].push_back(node);
								}
							}
						}
					}
				}

				ll = (ll + 1);
			}
			LLVM_DEBUG(dbgs() << "MapASAPLevel:: nodeIdx=" << node->getIdx() << " ,Possible Dests = " << nodeDestMap[node].size() << "\n");
			for (int i = 0; i < nodeDestMap[node].size(); ++i)
			{
				LLVM_DEBUG(dbgs() << nodeDestMap[node][i].first->getName() << ",RT=" << nodeDestMap[node][i].second << ";");
			}
			LLVM_DEBUG(dbgs() << "\n");
		}

		LLVM_DEBUG(dbgs() << "MapASAPLevel:: Finding dests are done!\n");

		//Multiple Desination Nodes
		deadEndReached = false;
		if (!MapMultiDestRec(&nodeDestMap, &destNodeMap, nodeDestMap.begin(), *(currCGRA->getCGRAEdges()), 0))
		{
			delete currCGRA;
			for (int var = 0; var < NodeList.size(); ++var)
			{
				node = NodeList[var];
				node->setMappedLoc(NULL);
			}
			return false;
		}
		//			return true;
	}
	return true;
}

bool DFG::MapCGRA_SMART(int XDim, int YDim, ArchType arch, int bTrack, int initMII)
{

	this->initBtrack = bTrack;
	this->backtrackCounter = bTrack;
	this->setName(this->getName() + "_" + getArchName(arch) + "_" + std::to_string(XDim) + "_" + std::to_string(YDim) + "_BT" + std::to_string(initBtrack));
	std::string mapfileName = this->getName() + "_mapping.log";
	mappingOutFile.open(mapfileName.c_str());
	clock_t begin = clock();
	int MII = std::ceil((float)NodeList.size() / ((float)XDim * (float)YDim));
	int memMII = std::ceil((float)getMEMOpsToBePlaced() / ((float)YDim));
	findMaxRecDist();
	conMatArr.push_back(getConMat());

	LLVM_DEBUG(dbgs() << "MapCGRAsa:: Resource Constrained MII = " << MII << "\n");
	LLVM_DEBUG(dbgs() << "MapCGRAsa:: Recurrence Constrained MII = " << getMaxRecDist() << "\n");

	LLVM_DEBUG(dbgs() << "MapCGRAsa:: MEMNodes/TotalNodes = " << getMEMOpsToBePlaced() << "/" << NodeList.size() << "\n");
	LLVM_DEBUG(dbgs() << "MapCGRAsa:: MEM Constrained MII = " << memMII << "\n");
	mappingOutFile << "MapCGRAsa:: Number of nodes = " << NodeList.size() << ", Edges = " << edgeList.size() << "\n";
	mappingOutFile << "MapCGRAsa:: Resource Constrained MII = " << MII << "\n";
	mappingOutFile << "MapCGRAsa:: MEMNodes/TotalNodes = " << getMEMOpsToBePlaced() << "/" << NodeList.size() << "\n";
	mappingOutFile << "MapCGRAsa:: OutLoopMEMOps = " << getOutLoopMEMOps() << "\n";
	mappingOutFile << "MapCGRAsa:: MEM Constrained MII = " << memMII << "\n";
	mappingOutFile << "MapCGRAsa:: Recurrence Constrained MII = " << getMaxRecDist() << "\n";

	MII = std::max(std::max(MII, getMaxRecDist()), std::max(memMII, initMII));
	//	int initMII = MII;
	//	MII = 19;

	//Sanity Check
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getAncestors().size() > 5)
		{
			LLVM_DEBUG(dbgs() << "Cannot map applications that have Fan in nodes more than 5\n");
			return false;
		}
	}

	int latency = 0;

	while (1)
	{
		if (MapASAPLevel(MII, XDim, YDim, arch))
		{
			clock_t end = clock();
			double elapsed_time = double(end - begin) / CLOCKS_PER_SEC;
			LLVM_DEBUG(dbgs() << "MapCGRAsa :: Mapping success with MII = " << MII << "\n");

			for (int i = 0; i < NodeList.size(); ++i)
			{
				node = NodeList[i];
				if (node->getmappedRealTime() > latency)
				{
					latency = node->getmappedRealTime();
				}
			}

			mappingOutFile << "MapCGRAsa :: Mapping success with MII = " << MII << " with a latency of " << latency << "\n";

			//Printing out SMART_PATH Histogram
			mappingOutFile << "\n MapCGRAsa :: Beginning of SMART Path Histogram = \n";
			mappingOutFile << "Path Size \tNumber of Paths \tPredicatePaths\n";
			for (int i = 0; i <= astar->maxSMARTPathLength; ++i)
			{
				if (astar->SMARTPathHist.find(i) == astar->SMARTPathHist.end())
				{
					mappingOutFile << i << "\t\t\t0\t\t\t";
				}
				else
				{
					mappingOutFile << i << "\t\t\t" << astar->SMARTPathHist[i] << "\t\t\t";
				}

				if (astar->SMARTPredicatePathHist.find(i) == astar->SMARTPathHist.end())
				{
					mappingOutFile << "0\n";
				}
				else
				{
					mappingOutFile << astar->SMARTPredicatePathHist[i] << "\n";
				}
			}
			mappingOutFile << "\n MapCGRAsa :: End of SMART Path Histogram.\n\n";

			//Printing out PATH Histrogram
			mappingOutFile << "\n MapCGRAsa :: Beginning of Path Histogram = \n";
			mappingOutFile << "PathSize \tNumber of Paths \tCountRegConnections \tRegConnections \n";
			for (int i = 0; i <= astar->maxPathLength; ++i)
			{
				if (astar->PathHist.find(i) == astar->PathHist.end())
				{
					mappingOutFile << i << "\t\t\t0\n";
				}
				else
				{
					mappingOutFile << i << "\t\t\t" << astar->PathHist[i];
					mappingOutFile << "\t\t\t\t\t" << astar->PathSMARTPathCount[i];
					mappingOutFile << "\t\t\t\t\t (";
					for (int j = 0; j < astar->PathSMARTPaths[i].size(); j++)
					{
						mappingOutFile << astar->PathSMARTPaths[i][j] << ",";
					}
					mappingOutFile << ")\n";
				}
			}

			//Printing out mapped routes
			printOutSMARTRoutes();
			//			printTurns();

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
							if ((connections[c].SrcPort == R0) ||
									(connections[c].SrcPort == R1) ||
									(connections[c].SrcPort == R2) ||
									(connections[c].SrcPort == R3))
							{
								if (connections[c].mappedDFGEdge == NULL)
								{
									count++;
								}
							}
						}
						mappingOutFile << "(" << t << "," << y << "," << x << ")"
								<< "=" << count << "\n";
						count = 0;
					}
				}
			}

			mappingOutFile << "Duration :: " << elapsed_time << "\n";
			break;
		}
		LLVM_DEBUG(dbgs() << "MapCGRAsa :: Mapping failed with MII = " << MII << "\n");
		mappingOutFile << "MapCGRAsa :: Mapping failed with MII = " << MII << "\n";
		if (MII >= 25)
		{
			LLVM_DEBUG(dbgs() << "Largest MII is 25, current MII = 25 , therefore aborting mapping\n");
			return false;
		}

		double currTime = double(clock() - begin) / CLOCKS_PER_SEC;
		//TODO : DAC18 removed time limit
		//		if(currTime > 60*15){
		//			LLVM_DEBUG(dbgs() << "8 mins window expired for the compilation, exiting\n";
		//			return false;
		//		}

		MII++;
	}
	mappingOutFile.close();
	return true;
}

void DFG::MapCGRA(int XDim, int YDim)
{

	//TODO : Add recurrence constrained MII
	int MII = std::ceil((float)NodeList.size() / ((float)XDim * (float)YDim));
	std::vector<ConnectedCGRANode> candidateCGRANodes;
	dfgNode *temp;

	struct cand
	{
		int max = 0;
		int curr = 0;

		cand(int m, int c) : max(m), curr(c) {}
	};

	std::vector<cand> chosenCandidates;

	while (1)
	{
		LLVM_DEBUG(dbgs() << "Mapping started with MII = " << MII << "\n");
		currCGRA = new CGRA(MII, XDim, YDim, REGS_PER_NODE);
		int nodeListSequencer = 0;
		int min_nodeListSequencer = NodeList.size();

		for (int i = 0; i < NodeList.size(); ++i)
		{
			chosenCandidates.push_back(cand(0, 0));
		}

		while (1)
		{
			temp = NodeList[nodeListSequencer];
			LLVM_DEBUG(dbgs() << "Mapping " << nodeListSequencer << "/" << NodeList.size() << "..."
					<< ", with MII = " << MII << "\n");

			if (nodeListSequencer == NodeList.size())
			{
				LLVM_DEBUG(dbgs() << "Mapping done...\n");
				break;
			}

			LLVM_DEBUG(dbgs() << "Finding candidates for NodeSeq =" << nodeListSequencer << "...\n");
			candidateCGRANodes = FindCandidateCGRANodes(temp);
			LLVM_DEBUG(dbgs() << "Found candidates : " << candidateCGRANodes.size() << "\n");

			//			for (int i = 0; i < candidateCGRANodes.size(); ++i) {
			//				LLVM_DEBUG(dbgs() << "(t,y,x) = (" << candidateCGRANodes[i].node->getT() << ","
			//										<< candidateCGRANodes[i].node->getY() << ","
			//										<< candidateCGRANodes[i].node->getX() << ")\n";
			//			}

			//Remove this special Debug
			//			if(nodeListSequencer == 28){
			//				if(currCGRA->getCGRANode(2,2,1)->getmappedDFGNode() == NULL){
			//					LLVM_DEBUG(dbgs() << "SPECIAL_DEBUG : (221) is NULL\n";
			//				}
			//				else{
			//					LLVM_DEBUG(dbgs() << "SPECIAL_DEBUG : (221) is "<< currCGRA->getCGRANode(2,2,1)->getmappedDFGNode()->getIdx() <<"\n";
			//				}
			//			}

			if (candidateCGRANodes.size() == 0)
			{
				do
				{
					chosenCandidates[nodeListSequencer].curr = 0;
					nodeListSequencer--;

					if (nodeListSequencer >= 0)
					{
						backTrack(nodeListSequencer);
					}

					LLVM_DEBUG(dbgs() << "nodeListSequencer =" << nodeListSequencer << "\n");
				} while ((chosenCandidates[nodeListSequencer].curr + 1 >= chosenCandidates[nodeListSequencer].max) && (nodeListSequencer >= 0));
				LLVM_DEBUG(dbgs() << "Backtracked to nodeListSequencer =" << nodeListSequencer << "\n");

				if (nodeListSequencer < 0)
				{
					LLVM_DEBUG(dbgs() << "Mapping failed for MII = " << MII << "...\n");
					break;
				}

				chosenCandidates[nodeListSequencer].curr = chosenCandidates[nodeListSequencer].curr + 1;
			}
			else
			{
				LLVM_DEBUG(dbgs() << "current candidate = " << chosenCandidates[nodeListSequencer].curr << "\n");
				chosenCandidates[nodeListSequencer].max = candidateCGRANodes.size();
				candidateCGRANodes[chosenCandidates[nodeListSequencer].curr].node->setMappedDFGNode(temp);
				temp->setMappedLoc(candidateCGRANodes[chosenCandidates[nodeListSequencer].curr].node);

				LLVM_DEBUG(dbgs() << "Mapped node with sequence =" << nodeListSequencer << "\n");
				LLVM_DEBUG(dbgs() << "(t,y,x) = (" << candidateCGRANodes[chosenCandidates[nodeListSequencer].curr].node->getT() << ","
						<< candidateCGRANodes[chosenCandidates[nodeListSequencer].curr].node->getY() << ","
						<< candidateCGRANodes[chosenCandidates[nodeListSequencer].curr].node->getX() << ")\n");
				//				LLVM_DEBUG(dbgs() << "(t,y,x) = (" << currCGRA->getCGRANode(temp->getMappedLoc()->getT(),temp->getMappedLoc()->getY(),temp->getMappedLoc()->getX())->getT()  << ","
				//									    << currCGRA->getCGRANode(temp->getMappedLoc()->getT(),temp->getMappedLoc()->getY(),temp->getMappedLoc()->getX())->getY()   << ","
				//									    << currCGRA->getCGRANode(temp->getMappedLoc()->getT(),temp->getMappedLoc()->getY(),temp->getMappedLoc()->getX())->getX()   << ")\n";
				nodeListSequencer++;
			}
			//			delete(&candidateCGRANodes);
		}

		if (nodeListSequencer == NodeList.size())
		{
			LLVM_DEBUG(dbgs() << "Mapping done...\n");
			break;
		}

		MII++;
		delete (currCGRA);
	}
}
