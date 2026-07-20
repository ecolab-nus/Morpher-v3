#include "DfgCoreInternal.h"

MemOp DFG::isMemoryOp(dfgNode *node)
{

	LoadInst *Ld = dyn_cast<LoadInst>(node->getNode());
	StoreInst *St = dyn_cast<StoreInst>(node->getNode());

	if (!St && !Ld)
	{
		return INVALID;
	}

	if (Ld)
	{
		if (!Ld->isSimple())
		{
			return INVALID;
		}
		else
		{
			return LOAD;
		}
	}

	if (St)
	{
		if (!St->isSimple())
		{
			return INVALID;
		}
		else
		{
			return STORE;
		}
	}

	return INVALID;
}

#ifndef XMLCheckResult
#define XMLCheckResult(a_eResult)           \
		if (a_eResult != tinyxml2::XML_SUCCESS) \
		{                                       \
			printf("Error: %i\n", a_eResult);   \
			return a_eResult;                   \
		}
#endif

#ifndef XMLCheckNULL
#define XMLCheckNULL(xmlNode) \
		if (xmlNode == NULL)      \
		return tinyxml2::XML_ERROR_FILE_READ_ERROR;
#endif

int DFG::readXML(std::string fileName)
{
	tinyxml2::XMLDocument inputDFG;
	std::vector<dfgNode *> nodelist;

	int totalNumberOps = -1;
	int opManualCount = 0;
	int totalNumberEdges = -1;
	int edgeManualCount = 0;

	tinyxml2::XMLError err;
	tinyxml2::XMLElement *nextElem;
	tinyxml2::XMLElement *inElem1;
	tinyxml2::XMLElement *inElem2;
	tinyxml2::XMLNode *pRoot;
	int ancNumber = -1;
	int childNumber = -1;

	int tempInt1 = -1;
	int tempInt2 = -1;
	const char *tempchararr;
	//	std::string tempString;

	LLVM_DEBUG(dbgs() << "Reading xml input file : " << fileName << "\n");
	err = inputDFG.LoadFile(fileName.c_str());
	XMLCheckResult(err);

	pRoot = inputDFG.FirstChild();
	XMLCheckNULL(pRoot);
	LLVM_DEBUG(dbgs() << pRoot->Value() << "\n");

	LLVM_DEBUG(dbgs() << "Reading OPs\n");
	nextElem = pRoot->FirstChildElement("OPs");
	XMLCheckNULL(nextElem);

	LLVM_DEBUG(dbgs() << "Reading OP-number\n");
	nextElem = nextElem->FirstChildElement("OP-number");
	XMLCheckNULL(nextElem);
	err = nextElem->QueryIntText(&totalNumberOps);
	XMLCheckResult(err);

	nextElem = nextElem->NextSiblingElement("OP");
	XMLCheckNULL(nextElem);

	while (nextElem != NULL)
	{
		dfgNode *node = new dfgNode(this);
		LLVM_DEBUG(dbgs() << "Reading OP," << opManualCount << "\n");

		inElem1 = nextElem->FirstChildElement("ID");
		XMLCheckNULL(inElem1);
		err = inElem1->QueryIntText(&tempInt1);
		XMLCheckResult(err);
		node->setIdx(tempInt1);

		inElem1 = nextElem->FirstChildElement("OP-type");
		XMLCheckNULL(inElem1);
		tempchararr = inElem1->GetText();
		std::string tempString = tempchararr;
		LLVM_DEBUG(dbgs() << tempchararr << "\n");
		node->setNameType(tempString);

		//		LLVM_DEBUG(dbgs() << "Reading In-edge-number : ";
		//		inElem1 = nextElem->FirstChildElement("In-edge-number");
		//		XMLCheckNULL(inElem1);
		//		err = inElem1->QueryIntText(&tempInt1);
		//		XMLCheckResult(err);
		//		ancNumber = tempInt1;
		//		LLVM_DEBUG(dbgs() << ancNumber << "\n";
		//
		//		if(ancNumber > 0){
		//			//traverse to in-edges
		//			inElem1 = nextElem->FirstChildElement("In-edges");
		//			XMLCheckNULL(inElem1);
		//
		//			inElem1 = inElem1->FirstChildElement("Edge");
		//			XMLCheckNULL(inElem1);
		//			for (int i = 0; i < ancNumber; ++i) {
		//				LLVM_DEBUG(dbgs() << "Read in-edge \n";
		//				err = inElem1->QueryIntText(&tempInt1);
		//				XMLCheckResult(err);
		//				node->InEdgesIdx.push_back(tempInt1);
		//
		//				inElem1 = inElem1->NextSiblingElement("Edge");
		//			}
		//		}
		//
		//		LLVM_DEBUG(dbgs() << "Reading Out-edge-number : \n";
		//		inElem1 = nextElem->FirstChildElement("Out-edge-number");
		//		XMLCheckNULL(inElem1);
		//		err = inElem1->QueryIntText(&tempInt1);
		//		XMLCheckResult(err);
		//		childNumber = tempInt1;
		//
		//		if(childNumber > 0){
		//			//traverse to in-edges
		//			inElem1 = nextElem->FirstChildElement("Out-edges");
		//			XMLCheckNULL(inElem1);
		//
		//			inElem1 = inElem1->FirstChildElement("Edge");
		//			XMLCheckNULL(inElem1);
		//			for (int i = 0; i < ancNumber; ++i) {
		//				err = inElem1->QueryIntText(&tempInt1);
		//				XMLCheckResult(err);
		//				node->OutEdgesIdx.push_back(tempInt1);
		//
		//				inElem1 = inElem1->NextSiblingElement("Edge");
		//			}
		//		}
		nextElem = nextElem->NextSiblingElement("OP");
		opManualCount++;
		nodelist.push_back(node);
	}

	assert(opManualCount == totalNumberOps);

	//EDGES
	LLVM_DEBUG(dbgs() << "Reading EDGEs\n");
	nextElem = pRoot->FirstChildElement("EDGEs");
	XMLCheckNULL(nextElem);

	LLVM_DEBUG(dbgs() << "Reading Edge-number\n");
	nextElem = nextElem->FirstChildElement("Edge-number");
	XMLCheckNULL(nextElem);
	err = nextElem->QueryIntText(&totalNumberEdges);
	XMLCheckResult(err);

	nextElem = nextElem->NextSiblingElement("EDGE");
	XMLCheckNULL(nextElem);

	while (nextElem != NULL)
	{
		LLVM_DEBUG(dbgs() << "Reading Edge, " << edgeManualCount << "\n");
		inElem1 = nextElem->FirstChildElement("Start-OP");
		XMLCheckNULL(inElem1);
		err = inElem1->QueryIntText(&tempInt1);
		XMLCheckResult(err);
		inElem1 = nextElem->FirstChildElement("End-OP");
		XMLCheckNULL(inElem1);
		err = inElem1->QueryIntText(&tempInt2);
		XMLCheckResult(err);

		for (int i = 0; i < nodelist.size(); ++i)
		{
			if (nodelist[i]->getIdx() == tempInt1)
			{
				for (int j = 0; j < nodelist.size(); ++j)
				{
					if (nodelist[j]->getIdx() == tempInt2)
					{
						nodelist[i]->addChildNode(nodelist[j]);
						nodelist[j]->addAncestorNode(nodelist[i]);

						Edge temp;
						temp.setID(this->getEdges().size());
						std::ostringstream ss;
						ss << std::dec << nodelist[i]->getIdx() << "_to_" << nodelist[j]->getIdx();
						temp.setName(ss.str());
						temp.setType(EDGE_TYPE_DATA);
						temp.setSrc(nodelist[i]);
						temp.setDest(nodelist[j]);
						this->InsertEdge(temp);
					}
				}
			}
		}
		nextElem = nextElem->NextSiblingElement("EDGE");
		edgeManualCount++;
	}

	assert(edgeManualCount == totalNumberEdges);
	this->setNodes(nodelist);
	this->setName(fileName);
	return 0;
}

int DFG::printREGIMapOuts()
{
	std::ofstream nodeFile;
	std::ofstream edgeFile;
	dfgNode *node;
	Edge *edge;
	std::string fName;
	std::hash<std::string> strHash;
	std::hash<const char *> charArrHash;

	//Printing Nodes
	fName = name + "_REGIMAP_nodefile.txt";
	nodeFile.open(fName.c_str());
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		if (node->getNode() != NULL)
		{
			nodeFile << std::to_string(node->getIdx()) << "\t";
			nodeFile << std::to_string((int)charArrHash(node->getNode()->getOpcodeName())) << "\t";
			nodeFile << node->getNode()->getOpcodeName() << std::endl;
		}
		else
		{
			nodeFile << std::to_string(node->getIdx()) << "\t";
			nodeFile << std::to_string((int)strHash(node->getNameType())) << "\t";
			nodeFile << node->getNameType() << std::endl;
		}
	}
	nodeFile.close();

	//Printing Edges
	fName = name + "_REGIMAP_edgefile.txt";
	edgeFile.open(fName.c_str());
	for (int i = 0; i < edgeList.size(); ++i)
	{
		edge = &edgeList[i];

		//currenlty assuming all the edges are data
		assert(edge->getType() == EDGE_TYPE_DATA);

		edgeFile << std::to_string(edge->getSrc()->getIdx()) << "\t";
		edgeFile << std::to_string(edge->getDest()->getIdx()) << "\t";
		edgeFile << "0"
				<< "\t";
		edgeFile << "TRU" << std::endl;
	}
	edgeFile.close();

	return 0;
}

int DFG::printTurns()
{
	dfgNode *node;
	dfgNode *parent;
	CGRANode *cnode;
	CGRANode *nextCnode;

	int xdiff;
	int ydiff;

	std::map<dfgNode *, std::vector<pathData *>> parentRouteMap;
	std::map<dfgNode *, std::vector<pathData *>>::iterator parentRouteMapIt;

	enum TurnDirs
	{
		NORTH,
		EAST,
		WEST,
		SOUTH,
		TILE
	};
	std::map<CGRANode *, std::map<TurnDirs, int>> CGRANodeTurnStatsMap;

	for (int t = 0; t < currCGRA->getMII(); ++t)
	{
		for (int y = 0; y < currCGRA->getYdim(); ++y)
		{
			for (int x = 0; x < currCGRA->getXdim(); ++x)
			{
				CGRANodeTurnStatsMap[currCGRA->getCGRANode(t, y, x)][NORTH] = 0;
				CGRANodeTurnStatsMap[currCGRA->getCGRANode(t, y, x)][EAST] = 0;
				CGRANodeTurnStatsMap[currCGRA->getCGRANode(t, y, x)][WEST] = 0;
				CGRANodeTurnStatsMap[currCGRA->getCGRANode(t, y, x)][SOUTH] = 0;
				CGRANodeTurnStatsMap[currCGRA->getCGRANode(t, y, x)][TILE] = 0;
			}
		}
	}

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		parentRouteMap = node->getMergeRoutingLocs();

		for (parentRouteMapIt = parentRouteMap.begin();
				parentRouteMapIt != parentRouteMap.end();
				++parentRouteMapIt)
		{

			parent = parentRouteMapIt->first;

			if (parentRouteMap[parent].size() > 0)
			{
				CGRANodeTurnStatsMap[parentRouteMap[parent][0]->cnode][TILE]++;
			}
			else
			{
				continue;
			}

			for (int j = 1; j < parentRouteMap[parent].size(); ++j)
			{
				cnode = parentRouteMap[parent][j]->cnode;
				nextCnode = parentRouteMap[parent][j - 1]->cnode;

				if (cnode->getT() == nextCnode->getT())
				{ //SMART Routes
					xdiff = nextCnode->getX() - cnode->getX();
					ydiff = nextCnode->getY() - cnode->getY();

					//either one should not be zero
					assert((xdiff != 0) || (ydiff != 0));

					if (ydiff > 0)
					{
						assert(xdiff == 0);
						CGRANodeTurnStatsMap[cnode][SOUTH]++;
					}
					else if (ydiff == 0)
					{
						if (xdiff > 0)
						{
							CGRANodeTurnStatsMap[cnode][EAST]++;
						}
						else
						{ // xdiff < 0
							CGRANodeTurnStatsMap[cnode][WEST]++;
						}
					}
					else
					{ //ydiff < 0
						assert(xdiff == 0);
						CGRANodeTurnStatsMap[cnode][NORTH]++;
					}
				}
				else
				{
					if (std::find(cnode->regAllocation[nextCnode].begin(), cnode->regAllocation[nextCnode].end(), findEdge(parent, node)) == cnode->regAllocation[nextCnode].end())
					{
						cnode->regAllocation[nextCnode].push_back(findEdge(parent, node));
					}
				}
			}
		}
	}

	std::ofstream outFile;
	std::string fName = name + "_TURNSTATS.csv";
	outFile.open(fName.c_str());

	outFile << "Cycle,Y,X,NORTH,EAST,WEST,SOUTH,TILE" << std::endl;
	for (int y = 0; y < currCGRA->getYdim(); ++y)
	{
		for (int x = 0; x < currCGRA->getXdim(); ++x)
		{
			for (int t = 0; t < currCGRA->getMII(); ++t)
			{
				cnode = currCGRA->getCGRANode(t, y, x);
				outFile << cnode->getName() << ",";
				outFile << std::to_string(CGRANodeTurnStatsMap[cnode][NORTH]) << ",";
				outFile << std::to_string(CGRANodeTurnStatsMap[cnode][EAST]) << ",";
				outFile << std::to_string(CGRANodeTurnStatsMap[cnode][WEST]) << ",";
				outFile << std::to_string(CGRANodeTurnStatsMap[cnode][SOUTH]) << ",";
				outFile << std::to_string(CGRANodeTurnStatsMap[cnode][TILE]) << std::endl;
			}
		}
	}
	outFile.close();

	//calling Reg stats
	printRegStats();

	return 0;
}

int DFG::printRegStats()
{

	std::ofstream outFileRegStats;
	std::ofstream outFileRegToggle;

	std::string fName = name + "_REGSTATS.csv";
	outFileRegStats.open(fName.c_str());
	fName = name + "_REGTOGGLE.csv";
	outFileRegToggle.open(fName.c_str());

	std::map<CGRANode *, std::vector<Edge *>>::iterator it;

	CGRANode *cnode;
	CGRANode *nextCnode;
	std::map<int, Edge *> regAlloc;
	std::map<int, int> regToggle;
	std::vector<Edge *> tempEdgeVec;
	std::vector<Edge *>::iterator tempEdgeVecIt;

	int freeSlots;
	int totalToggles = NodeList.size(); //This is for output register of the ALU
	const int maxToggles = currCGRA->getXdim() * currCGRA->getYdim() * currCGRA->getMII() * (currCGRA->getRegsPerNode() + 1);

	for (int y = 0; y < currCGRA->getYdim(); ++y)
	{
		for (int x = 0; x < currCGRA->getXdim(); ++x)
		{
			outFileRegToggle << currCGRA->getCGRANode(0, y, x)->getNameWithOutTime() << ",";

			//Init regAlloc
			for (int i = 0; i < currCGRA->getRegsPerNode(); ++i)
			{
				regAlloc[i] = NULL;
				regToggle[i] = 0;
			}

			for (int t = 0; t < currCGRA->getMII(); ++t)
			{
				cnode = currCGRA->getCGRANode(t, y, x);
				outFileRegStats << cnode->getName() << ",";

				for (it = cnode->regAllocation.begin();
						it != cnode->regAllocation.end();
						it++)
				{

					nextCnode = it->first;
					if (nextCnode->getT() != cnode->getT())
					{
						tempEdgeVec = it->second;

						if (tempEdgeVec.size() > 4)
						{
							LLVM_DEBUG(dbgs() << "init tempEdgeVec.size() =" << tempEdgeVec.size() << "\n");
							for (int i = 0; i < tempEdgeVec.size(); ++i)
							{
								LLVM_DEBUG(dbgs() << tempEdgeVec[i]->getName() << "\n");
							}
						}

						freeSlots = 0;
						for (int i = 0; i < currCGRA->getRegsPerNode(); ++i)
						{
							if (regAlloc[i] != NULL)
							{
								tempEdgeVecIt = std::find(tempEdgeVec.begin(), tempEdgeVec.end(), regAlloc[i]);
								if (tempEdgeVecIt != tempEdgeVec.end())
								{
									tempEdgeVec.erase(tempEdgeVecIt);
								}
								else
								{
									regAlloc[i] = NULL;
									freeSlots++;
								}
							}
							else
							{
								freeSlots++;
							}
						}

						if (freeSlots < tempEdgeVec.size())
						{
							LLVM_DEBUG(dbgs() << "freeSlots=" << freeSlots << ",tempEdgeVec.size()=" << tempEdgeVec.size() << "\n");
						}
						assert(freeSlots >= tempEdgeVec.size());

						for (int i = 0; i < tempEdgeVec.size(); ++i)
						{
							for (int j = 0; j < currCGRA->getRegsPerNode(); ++j)
							{
								if (regAlloc[j] == NULL)
								{
									regAlloc[j] = tempEdgeVec[i];
									regToggle[j]++;
									totalToggles++;
									break;
								}
							}
						}

						for (int i = 0; i < it->second.size(); ++i)
						{
							outFileRegStats << it->second[i]->getName();
							for (int j = 0; j < currCGRA->getRegsPerNode(); ++j)
							{
								if (regAlloc[j] == it->second[i])
								{
									outFileRegStats << "-R" << std::to_string(j) << ",";
									break;
								}
							}
						}
						//						outFile << it->second->getName() << ",";
					}
				}
				outFileRegStats << std::endl;
			}

			for (int i = 0; i < currCGRA->getRegsPerNode(); ++i)
			{
				outFileRegToggle << std::to_string(regToggle[i]) << ",";
			}
			outFileRegToggle << std::endl;
		}
	}

	outFileRegToggle << std::endl;
	outFileRegToggle << "TOTAL Toggles," << std::to_string(totalToggles) << std::endl;
	outFileRegToggle << "Max Toggles," << std::to_string(maxToggles) << std::endl;
	outFileRegToggle << "Toggle Percentage," << std::to_string((double(totalToggles) * 100.0) / double(maxToggles)) << std::endl;

	outFileRegStats.close();
	outFileRegToggle.close();
	return 0;
}

int DFG::sortPossibleDests(
		std::vector<std::pair<CGRANode *, int>> *possibleDests)
{
	return 0;
}

int DFG::removeRedEdgesPHI()
{
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getNode() == NULL)
			continue;

		if (dyn_cast<PHINode>(node->getNode()))
		{
			for (dfgNode *parent : node->getAncestors())
			{
				if (node->getNameType().compare("CMERGE") != 0)
				{
					parent->removeChild(node);
					node->removeAncestor(parent);
					removeEdge(findEdge(parent, node));
				}
			}
		}
	}
	return 0;
}

int DFG::addCMERGEtoSELECT()
{
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getNode() == NULL)
			continue;

		if (SelectInst *SEL = dyn_cast<SelectInst>(node->getNode()))
		{
			LLVM_DEBUG(SEL->dump());
			LLVM_DEBUG(dbgs() << "node ancestors = " << node->getAncestors().size() << "\n");
			//			assert(node->getAncestors().size()==3);

			int condVal;
			dfgNode *condNode = NULL;
			//			if(ConstantInt *CI = dyn_cast<ConstantInt>(SEL->getCondition())){
			//				condVal = CI->getSExtValue();
			//			}
			if (dyn_cast<Instruction>(SEL->getCondition()))
			{
				condNode = findNode(cast<Instruction>(SEL->getCondition()));
				LLVM_DEBUG(cast<Instruction>(SEL->getCondition())->dump());
				assert(condNode != NULL);
				assert(std::find(node->getAncestors().begin(), node->getAncestors().end(), condNode) != node->getAncestors().end());
			}
			else
			{
				assert(false);
			}

			int trueVal;
			dfgNode *trueNode = NULL;
			if (ConstantInt *CI = dyn_cast<ConstantInt>(SEL->getTrueValue()))
			{
				trueVal = CI->getSExtValue();
			}
			else if (ConstantFP *FP = dyn_cast<ConstantFP>(SEL->getTrueValue()))
			{
				trueVal = 0; // 2019 work
			}
			else if (dyn_cast<Instruction>(SEL->getTrueValue()))
			{
				trueNode = findNode(cast<Instruction>(SEL->getTrueValue()));
				LLVM_DEBUG(cast<Instruction>(SEL->getTrueValue())->dump());
				assert(trueNode != NULL);
				assert(std::find(node->getAncestors().begin(), node->getAncestors().end(), trueNode) != node->getAncestors().end());
			}
			else
			{
				assert(false);
			}

			int falseVal;
			dfgNode *falseNode = NULL;
			if (ConstantInt *CI = dyn_cast<ConstantInt>(SEL->getFalseValue()))
			{
				falseVal = CI->getSExtValue();
			}
			else if (ConstantFP *FP = dyn_cast<ConstantFP>(SEL->getFalseValue()))
			{
				falseVal = 0; // 2019 work
			}
			else if (dyn_cast<Instruction>(SEL->getFalseValue()))
			{
				falseNode = findNode(cast<Instruction>(SEL->getFalseValue()));
				LLVM_DEBUG(cast<Instruction>(SEL->getFalseValue())->dump());
				assert(falseNode != NULL);
				assert(std::find(node->getAncestors().begin(), node->getAncestors().end(), falseNode) != node->getAncestors().end());
			}
			else
			{
				assert(false);
			}

			condNode->removeChild(node);
			node->removeAncestor(condNode);
			removeEdge(findEdge(condNode, node));

			if (trueNode)
			{
				trueNode->removeChild(node);
				node->removeAncestor(trueNode);
				removeEdge(findEdge(trueNode, node));
				node->addCMergeParent(condNode, trueNode, -1, true);
			}
			else
			{
				node->addCMergeParent(condNode, trueNode, trueVal, true);
			}

			dfgNode *notnode = new dfgNode(this);
			notnode->setIdx(this->getNodesPtr()->size());
			this->getNodesPtr()->push_back(notnode);
			notnode->BB = node->BB;
			notnode->setNameType("XORNOT");

			condNode->addChildNode(notnode);
			notnode->addAncestorNode(condNode);

			if (falseNode)
			{
				falseNode->removeChild(node);
				node->removeAncestor(falseNode);
				removeEdge(findEdge(falseNode, node));
				node->addCMergeParent(notnode, falseNode, -1, true);
			}
			else
			{
				node->addCMergeParent(notnode, falseNode, falseVal, true);
			}
		}
	}
	return 0;
}

int DFG::printJUMPLHeader(std::ofstream &binFile,
		std::ofstream &binOpNameFile)
{

	binOp currBinOp;

	currBinOp.opcode = HyCUBEInsBinary[JUMPL];
	currBinOp.outMap[PRED] = 0b111;
	currBinOp.outMap[OP1] = 0b111;
	currBinOp.outMap[OP2] = 0b111;
	currBinOp.outMap[NORTH] = 0b111;
	currBinOp.outMap[EAST] = 0b111;
	currBinOp.outMap[SOUTH] = 0b111;
	currBinOp.outMap[WEST] = 0b111;
	currBinOp.regwen = 0;
	currBinOp.regbypass = 0;
	currBinOp.tregwen = 0;

	uint32_t constant = 1;										 //PC
	constant = (constant << 5) | (currCGRA->getMII() & 0b11111); //LE
	constant = (constant << 5) | 1;
	currBinOp.constant = constant;
	currBinOp.constantValid = 1;
	currBinOp.npb = 0;

	binFile << std::to_string(0) << std::endl;
	binOpNameFile << std::to_string(0) << std::endl;

	for (int j = 0; j < currCGRA->getYdim(); ++j)
	{
		for (int k = 0; k < currCGRA->getXdim(); ++k)
		{
			binFile << "Y=" << std::to_string(j) << " X=" << std::to_string(k) << ",";
			binOpNameFile << "Y=" << std::to_string(j) << " X=" << std::to_string(k) << ",";

			//Print Binary
			binFile << std::setfill('0');
			binFile << std::setw(1) << std::bitset<1>(currBinOp.npb);			//<< ",";
			binFile << std::setw(1) << std::bitset<1>(currBinOp.constantValid); //<< ",";
			binFile << std::setw(27) << std::bitset<27>(currBinOp.constant);	//<< ",";
			binFile << std::setw(5) << std::bitset<5>(currBinOp.opcode);		//<< "," ;
			binFile << std::setw(4) << std::bitset<4>(currBinOp.regwen);		//<< ",";
			binFile << std::bitset<1>(currBinOp.tregwen);						//<< ",";
			binFile << std::setw(4) << std::bitset<4>(currBinOp.regbypass);		//<< ",";
			binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[PRED]);  //<< ",";
			binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP2]);   //<< ",";
			binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP1]);   //<< ",";
			binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[NORTH]); //<< ",";
			binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[WEST]);  //<< ",";
			binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[SOUTH]); //<< ",";
			binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[EAST]) << std::endl;

			//Print Binary with OpName
			binOpNameFile << std::setfill('0');
			binOpNameFile << std::setw(1) << std::bitset<1>(currBinOp.npb);			  //<< ",";
			binOpNameFile << std::setw(1) << std::bitset<1>(currBinOp.constantValid); //<< ",";
			binOpNameFile << std::setw(27) << std::bitset<27>(currBinOp.constant);	//<< ",";
			binOpNameFile << std::setw(5) << std::bitset<5>(currBinOp.opcode);		  //<< "," ;
			binOpNameFile << std::setw(4) << std::bitset<4>(currBinOp.regwen);		  //<< ",";
			binOpNameFile << std::bitset<1>(currBinOp.tregwen);						  //<< ",";
			binOpNameFile << std::setw(4) << std::bitset<4>(currBinOp.regbypass);	 //<< ",";
			binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[PRED]);  //<< ",";
			binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP2]);   //<< ",";
			binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP1]);   //<< ",";
			binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[NORTH]); //<< ",";
			binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[WEST]);  //<< ",";
			binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[SOUTH]); //<< ",";
			binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[EAST]) << ",";
			binOpNameFile << HyCUBEInsStrings[JUMPL] << std::endl;
		}
	}

	binFile << std::endl;
	binOpNameFile << std::endl;

	return 0;
}

int DFG::printMapping()
{
	dfgNode *node;
	dfgNode *parent;
	CGRANode *cnode;
	CGRANode *PrevCnode;
	CGRANode *PrevPrevCnode;
	std::vector<CGRAEdge> cgraEdges;
	std::vector<CGRAEdge> cgraEdges_t;
	std::vector<CGRAEdge> cgraEdges_t2;
	std::map<Port, Edge *> portDfgEdgeMap;

	std::vector<Port> portOrder = {NORTH, EAST, WEST, SOUTH, R0, R1, R2, R3};
	std::vector<Port> insPortOrder = {TREG, R0, R1, R2, R3, PRED, OP1, OP2, NORTH, EAST, WEST, SOUTH};
	std::map<CGRANode *, std::map<Port, Port>> XBarMap;

	//Check all nodes are mapped
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		assert(node->getMappedLoc() != NULL);
	}

	std::ofstream mapFile;
	std::string mapFileName = name + "_mapFile.csv";
	mapFile.open(mapFileName.c_str());

	std::ofstream insFile;
	std::string insFileName = name + "_insFile.csv";
	insFile.open(insFileName.c_str());

	std::ofstream binFile;
	std::string binFileName = name + "_binFile.trc";
	binFile.open(binFileName.c_str());

	std::ofstream binOpNameFile;
	std::string binOpNameFileFileName = name + "_binOpNameFile.trc";
	binOpNameFile.open(binOpNameFileFileName.c_str());

	//Print Header
	mapFile << "Time,";
	insFile << "Time,";
	//	binFile << "Time,";
	for (int j = 0; j < currCGRA->getYdim(); ++j)
	{
		for (int k = 0; k < currCGRA->getXdim(); ++k)
		{
			mapFile << "Y=" << std::to_string(j) << " X=" << std::to_string(k) << ",";
			mapFile << "NORTH,EAST,WEST,SOUTH,R0,R1,R2,R3,";

			insFile << "Y=" << std::to_string(j) << " X=" << std::to_string(k) << ",";
			insFile << "TREG,R0,R1,R2,R3,PRED,OP1,OP2,NORTH,EAST,WEST,SOUTH,";

			//			binFile << "Y=" << std::to_string(j) << " X=" << std::to_string(k) << ",";
		}
	}
	binFile << "NPB,CONSTVALID,CONST,OPCODE,REGWEN,TREGWEN,REGBYPASS,PRED,OP1,OP2,NORTH,WEST,SOUTH,EAST";
	insFile << std::endl;
	mapFile << std::endl;
	binFile << std::endl;

	std::map<int, std::map<int, uint64_t>> constantValidMaskMap;

	printJUMPLHeader(binFile, binOpNameFile);

	//Print Data
	std::map<dfgNode *, std::set<dfgNode *>> parentInfo;
	for (int i = 0; i < currCGRA->getMII(); ++i)
	{
		mapFile << std::to_string(i) << ",";
		insFile << std::to_string(i) << ",";
		binFile << std::to_string(i + 1) << std::endl;
		binOpNameFile << std::to_string(i + 1) << std::endl;
		for (int j = 0; j < currCGRA->getYdim(); ++j)
		{
			for (int k = 0; k < currCGRA->getXdim(); ++k)
			{
				cnode = currCGRA->getCGRANode(i, j, k);
				node = cnode->getmappedDFGNode();
				if (node != NULL)
				{
					mapFile << std::to_string(node->getIdx()) << "::Parents<";
					for (int l = 0; l < node->getAncestors().size(); ++l)
					{
						parent = node->getAncestors()[l];
						mapFile << std::to_string(parent->getIdx()) << ":";
					}
					mapFile << ">,";
					insFile << HyCUBEInsStrings[node->getFinalIns()] << ",";
				}
				else
				{
					insFile << "NA,";
					mapFile << "NA,";
				}
				binFile << "Y=" << std::to_string(j) << " X=" << std::to_string(k) << ",";
				binOpNameFile << "Y=" << std::to_string(j) << " X=" << std::to_string(k) << ",";
				//CGRAEdges
				PrevCnode = cnode;
				PrevPrevCnode = currCGRA->getCGRANode((i - 1 + 3 * currCGRA->getMII()) % currCGRA->getMII(), j, k);
				cnode = currCGRA->getCGRANode((i + 1) % currCGRA->getMII(), j, k);
				cgraEdges = (*currCGRA->getCGRAEdges())[cnode];
				cgraEdges_t = currCGRA->getCGRAEdgesWithDest(cnode);

				for (int l = 0; l < cgraEdges.size(); ++l)
				{
					if (cgraEdges[l].mappedDFGEdge != NULL)
					{
						portDfgEdgeMap[cgraEdges[l].SrcPort] = cgraEdges[l].mappedDFGEdge;

						if (PrevPrevCnode->getPEType() == MEM)
						{
							if (cgraEdges[l].mappedDFGEdge->getSrc() == PrevPrevCnode->getmappedDFGNode())
							{
								XBarMap[PrevCnode][cgraEdges[l].SrcPort] = TILE;
							}
							else
							{
								for (int m = 0; m < cgraEdges_t.size(); ++m)
								{
									if (cgraEdges_t[m].mappedDFGEdge->getSrc() == cgraEdges[l].mappedDFGEdge->getSrc())
									{
										XBarMap[PrevCnode][cgraEdges[l].SrcPort] = cgraEdges_t[m].DstPort;
									}
								}
							}
						}
						else
						{
							if (cgraEdges[l].mappedDFGEdge->getSrc() == PrevCnode->getmappedDFGNode())
							{
								XBarMap[PrevCnode][cgraEdges[l].SrcPort] = TILE;
							}
							else
							{
								for (int m = 0; m < cgraEdges_t.size(); ++m)
								{
									if (cgraEdges_t[m].mappedDFGEdge->getSrc() == cgraEdges[l].mappedDFGEdge->getSrc())
									{
										XBarMap[PrevCnode][cgraEdges[l].SrcPort] = cgraEdges_t[m].DstPort;
									}
								}
							}
						}
					}
				}

				// TODO :: ADD a similiar if strucuture to MEM nodes where I need to check PrevPrevCnode instead of PrevCnode
				if (cnode->getmappedDFGNode() != NULL)
				{
					if (PrevCnode->getmappedDFGNode() != NULL)
					{
						for (int l = 0; l < cnode->getmappedDFGNode()->getAncestors().size(); ++l)
						{
							if (PrevCnode->getmappedDFGNode() == cnode->getmappedDFGNode()->getAncestors()[l])
							{
								dfgNode *currCnodeNode = cnode->getmappedDFGNode();
								dfgNode *cnodeParent = cnode->getmappedDFGNode()->getAncestors()[l];
								parentInfo[currCnodeNode].insert(cnodeParent);
								if (PrevCnode->getmappedDFGNode()->getFinalIns() == NOP)
								{

									//How can a NOP be a parent of someone?
									assert(false);

									//									if(XBarMap[PrevCnode].find(PRED) == XBarMap[PrevCnode].end()){
									//										XBarMap[PrevCnode][PRED] = TILE;
									//									}
									//									else if(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end()){
									//										XBarMap[PrevCnode][OP1] = TILE;
									//									}
									//									else if(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end()){
									//										XBarMap[PrevCnode][OP2] = TILE;
									//									}
									//									else{
									//										assert(false);
									//									}
								}
								else
								{

									bool found = false;
									if (currCnodeNode->parentClassification.find(0) !=
											currCnodeNode->parentClassification.end())
									{
										if (currCnodeNode->parentClassification[0] == cnodeParent)
										{
											assert(XBarMap[PrevCnode].find(PRED) == XBarMap[PrevCnode].end());
											XBarMap[PrevCnode][PRED] = TILE;
											found = true;
										}
									}
									else
									{
										XBarMap[PrevCnode][PRED] = INV;
									}

									if (currCnodeNode->parentClassification.find(1) !=
											currCnodeNode->parentClassification.end())
									{
										if (currCnodeNode->parentClassification[1] == cnodeParent)
										{
											assert(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end());
											XBarMap[PrevCnode][OP1] = TILE;
											found = true;
										}
									}
									else
									{
										XBarMap[PrevCnode][OP1] = INV;
									}

									if (currCnodeNode->parentClassification.find(2) !=
											currCnodeNode->parentClassification.end())
									{
										if (currCnodeNode->parentClassification[2] == cnodeParent)
										{
											assert(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end());
											XBarMap[PrevCnode][OP2] = TILE;
											found = true;
										}
									}
									else
									{
										XBarMap[PrevCnode][OP2] = INV;
									}

									LLVM_DEBUG(dbgs() << "printMapping : currNode = " << currCnodeNode->getIdx() << "\n");
									LLVM_DEBUG(dbgs() << "printMapping : currParent = " << cnodeParent->getIdx() << "\n");
									assert(found);

									//									if((PrevCnode->getmappedDFGNode()->getFinalIns() == CMP) ||
									//									   (PrevCnode->getmappedDFGNode()->getFinalIns() == BR)	){
									//										XBarMap[PrevCnode][PRED] = TILE;
									//									}
									//									else if(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end()){
									//										XBarMap[PrevCnode][OP1] = TILE;
									//									}
									//									else if(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end()){
									//										XBarMap[PrevCnode][OP2] = TILE;
									//									}
									//									else{
									//										LLVM_DEBUG(dbgs() << "UNCOMMENT here : assertion failes here truly!\n";
									////										assert(false);
									//									}
								}
							}
						}
					}
				}

				//adding the similiar structure
				if (PrevPrevCnode->getPEType() == MEM)
				{
					if (cnode->getmappedDFGNode() != NULL)
					{
						if (PrevPrevCnode->getmappedDFGNode() != NULL)
						{
							for (int l = 0; l < cnode->getmappedDFGNode()->getAncestors().size(); ++l)
							{
								if (PrevPrevCnode->getmappedDFGNode() == cnode->getmappedDFGNode()->getAncestors()[l])
								{

									dfgNode *currCnodeNode = cnode->getmappedDFGNode();
									dfgNode *cnodeParent = cnode->getmappedDFGNode()->getAncestors()[l];
									parentInfo[currCnodeNode].insert(cnodeParent);
									if (PrevPrevCnode->getmappedDFGNode()->getFinalIns() == NOP)
									{

										//How can a NOP be a parent of someone?
										assert(false);

										//									if(XBarMap[PrevCnode].find(PRED) == XBarMap[PrevCnode].end()){
										//										XBarMap[PrevCnode][PRED] = TILE;
										//									}
										//									else if(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end()){
										//										XBarMap[PrevCnode][OP1] = TILE;
										//									}
										//									else if(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end()){
										//										XBarMap[PrevCnode][OP2] = TILE;
										//									}
										//									else{
										//										assert(false);
										//									}
									}
									else
									{

										bool found = false;
										if (currCnodeNode->parentClassification.find(0) !=
												currCnodeNode->parentClassification.end())
										{
											if (currCnodeNode->parentClassification[0] == cnodeParent)
											{
												assert(XBarMap[PrevCnode].find(PRED) == XBarMap[PrevCnode].end());
												XBarMap[PrevCnode][PRED] = TILE;
												found = true;
											}
										}
										else
										{
											XBarMap[PrevCnode][PRED] = INV;
										}

										if (currCnodeNode->parentClassification.find(1) !=
												currCnodeNode->parentClassification.end())
										{
											if (currCnodeNode->parentClassification[1] == cnodeParent)
											{
												assert(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end());
												XBarMap[PrevCnode][OP1] = TILE;
												found = true;
											}
										}
										else
										{
											XBarMap[PrevCnode][OP1] = INV;
										}

										if (currCnodeNode->parentClassification.find(2) !=
												currCnodeNode->parentClassification.end())
										{
											if (currCnodeNode->parentClassification[2] == cnodeParent)
											{
												assert(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end());
												XBarMap[PrevCnode][OP2] = TILE;
												found = true;
											}
										}
										else
										{
											XBarMap[PrevCnode][OP2] = INV;
										}

										LLVM_DEBUG(dbgs() << "printMapping : currNode = " << currCnodeNode->getIdx() << "\n");
										LLVM_DEBUG(dbgs() << "printMapping : currParent = " << cnodeParent->getIdx() << "\n");

										if (!found)
										{
											LLVM_DEBUG(dbgs() << "Node : \n");
											if (currCnodeNode->getNode())
											{
												LLVM_DEBUG(currCnodeNode->getNode()->dump());
											}
											LLVM_DEBUG(dbgs() << "Parent : \n");
											if (cnodeParent->getNode())
											{
												LLVM_DEBUG(cnodeParent->getNode()->dump());
											}
											else if (cnodeParent->getNameType().compare("OUTOutLoopLOAD") == 0)
											{
												LLVM_DEBUG(OutLoopNodeMapReverse[cnodeParent]->dump());
											}
										}
										//TODO : DAC18
										//										assert(found);

										//									if((PrevCnode->getmappedDFGNode()->getFinalIns() == CMP) ||
										//									   (PrevCnode->getmappedDFGNode()->getFinalIns() == BR)	){
										//										XBarMap[PrevCnode][PRED] = TILE;
										//									}
										//									else if(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end()){
										//										XBarMap[PrevCnode][OP1] = TILE;
										//									}
										//									else if(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end()){
										//										XBarMap[PrevCnode][OP2] = TILE;
										//									}
										//									else{
										//										LLVM_DEBUG(dbgs() << "UNCOMMENT here : assertion failes here truly!\n";
										////										assert(false);
										//									}
									}
								}
							}
						}
					}
				}

				//removing other edges if edges with dest as current node is found
				cgraEdges_t2.clear();
				for (int m = 0; m < cgraEdges_t.size(); ++m)
				{

					if (cnode->getmappedDFGNode() == NULL)
						break;
					if (cnode->getmappedDFGNode()->isParent(cgraEdges_t[m].mappedDFGEdge->getSrc()))
					{
						cgraEdges_t2.push_back(cgraEdges_t[m]);
					}

					//					if(cgraEdges_t[m].mappedDFGEdge->getDest() == cnode->getmappedDFGNode()){
					//
					//					}
				}

				if (!cgraEdges_t2.empty())
				{
					cgraEdges_t = cgraEdges_t2;
				}

				for (int m = 0; m < cgraEdges_t.size(); ++m)
				{
					dfgNode *currCnodeNode = cnode->getmappedDFGNode();

					if (currCnodeNode == NULL)
					{
						continue;
					}

					if (parentInfo[currCnodeNode].find(cgraEdges_t[m].mappedDFGEdge->getSrc()) != parentInfo[currCnodeNode].end())
					{
						continue;
					}

					bool parentFound = false;
					for (dfgNode *parent : currCnodeNode->getAncestors())
					{
						currCGRA->printCGRAEdge(cgraEdges_t[m]);
						if (parent == cgraEdges_t[m].mappedDFGEdge->getSrc())
						{
							parentFound = true;
						}
					}
					//					bool parentFound = std::find(currCnodeNode->getAncestors().begin(),
					//												 currCnodeNode->getAncestors().end(),
					//												 cgraEdges_t[m].mappedDFGEdge->getSrc())
					//									   != currCnodeNode->getAncestors().end();
					if (parentFound)
					{
						LLVM_DEBUG(dbgs() << "currCnodeNode=" << currCnodeNode->getIdx() << "\n");
						LLVM_DEBUG(dbgs() << "Parents=");
						for (dfgNode *parent : currCnodeNode->getAncestors())
						{
							LLVM_DEBUG(dbgs() << parent->getIdx() << ",");
						}
						LLVM_DEBUG(dbgs() << "\n");
						LLVM_DEBUG(dbgs() << "Parent=" << cgraEdges_t[m].mappedDFGEdge->getSrc()->getIdx() << "\n");
					}

					if ((cgraEdges_t[m].mappedDFGEdge->getDest() == cnode->getmappedDFGNode()) || parentFound)
					{

						dfgNode *cnodeParent = cgraEdges_t[m].mappedDFGEdge->getSrc();
						dfgNode *cnodeChild = cgraEdges_t[m].mappedDFGEdge->getDest();
						parentInfo[currCnodeNode].insert(cnodeParent);

						if (cgraEdges_t[m].mappedDFGEdge->getSrc()->getFinalIns() == NOP)
						{

							assert(false);
							//Nothing should be routed from a NOP instruction

							//							if(XBarMap[PrevCnode].find(PRED) == XBarMap[PrevCnode].end()){
							//								XBarMap[PrevCnode][PRED] = cgraEdges_t[m].DstPort;
							//							}
							//							else if(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end()){
							//								XBarMap[PrevCnode][OP1] = cgraEdges_t[m].DstPort;
							//							}
							//							else if(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end()){
							//								XBarMap[PrevCnode][OP2] = cgraEdges_t[m].DstPort;
							//							}
							//							else{
							//								assert(false);
							//							}
						}
						else
						{

							LLVM_DEBUG(dbgs() << "printMapping : currNode = " << currCnodeNode->getIdx() << "\n");
							LLVM_DEBUG(dbgs() << "printMapping : currParent = " << cnodeParent->getIdx() << "\n");
							LLVM_DEBUG(dbgs() << "printMapping : PrevCnode = " << PrevCnode->getName() << "\n");

							bool found = false;
							if (currCnodeNode->parentClassification.find(0) !=
									currCnodeNode->parentClassification.end())
							{
								if (currCnodeNode->parentClassification[0] == cnodeParent)
								{
									this->getCGRA()->printCGRAEdge(cgraEdges_t[m]);
									assert(XBarMap[PrevCnode].find(PRED) == XBarMap[PrevCnode].end());
									XBarMap[PrevCnode][PRED] = cgraEdges_t[m].DstPort;
									LLVM_DEBUG(dbgs() << "PRED : " << cgraEdges_t[m].DstPort << "\n");
									found = true;
								}
							}
							else
							{
								XBarMap[PrevCnode][PRED] = INV;
							}

							if (currCnodeNode->parentClassification.find(1) !=
									currCnodeNode->parentClassification.end())
							{
								if (currCnodeNode->parentClassification[1] == cnodeParent)
								{
									this->getCGRA()->printCGRAEdge(cgraEdges_t[m]);
									assert(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end());
									XBarMap[PrevCnode][OP1] = cgraEdges_t[m].DstPort;
									LLVM_DEBUG(dbgs() << "I1 : " << cgraEdges_t[m].DstPort << "\n");
									found = true;
								}
							}
							else
							{
								XBarMap[PrevCnode][OP1] = INV;
							}

							if (currCnodeNode->parentClassification.find(2) !=
									currCnodeNode->parentClassification.end())
							{
								if (currCnodeNode->parentClassification[2] == cnodeParent)
								{
									this->getCGRA()->printCGRAEdge(cgraEdges_t[m]);
									assert(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end());
									XBarMap[PrevCnode][OP2] = cgraEdges_t[m].DstPort;
									LLVM_DEBUG(dbgs() << "I2 : " << cgraEdges_t[m].DstPort << "\n");
									found = true;
								}
							}
							else
							{
								XBarMap[PrevCnode][OP2] = INV;
							}

							if (!found)
							{
								LLVM_DEBUG(dbgs() << "Node : \n");
								if (currCnodeNode->getNode())
								{
									LLVM_DEBUG(currCnodeNode->getNode()->dump());
								}
								LLVM_DEBUG(dbgs() << "Parent : \n");
								if (cnodeParent->getNode())
								{
									LLVM_DEBUG(cnodeParent->getNode()->dump());
								}
								else if (cnodeParent->getNameType().compare("OUTOutLoopLOAD") == 0)
								{
									LLVM_DEBUG(OutLoopNodeMapReverse[cnodeParent]->dump());
								}
							}

							//TODO : DAC18
							//							assert(found);

							// mapped for hyCUBE instructions
							//							if((cgraEdges_t[m].mappedDFGEdge->getSrc()->getFinalIns() == CMP) ||
							//							   (cgraEdges_t[m].mappedDFGEdge->getSrc()->getFinalIns() == BR)	){
							//								XBarMap[PrevCnode][PRED] = cgraEdges_t[m].DstPort;
							//							}
							//							else if(XBarMap[PrevCnode].find(OP1) == XBarMap[PrevCnode].end()){
							//								XBarMap[PrevCnode][OP1] = cgraEdges_t[m].DstPort;
							//							}
							//							else if(XBarMap[PrevCnode].find(OP2) == XBarMap[PrevCnode].end()){
							//								XBarMap[PrevCnode][OP2] = cgraEdges_t[m].DstPort;
							//							}
							//							else{
							//								LLVM_DEBUG(dbgs() << "UNCOMMENT here : assertion failes here truly!\n";
							////								assert(false);
							//							}
						}
					}
				}

				//File Printing

				for (int l = 0; l < portOrder.size(); ++l)
				{
					if (portDfgEdgeMap.find(portOrder[l]) != portDfgEdgeMap.end())
					{
						mapFile << portDfgEdgeMap[portOrder[l]]->getName() << ",";
					}
					else
					{
						mapFile << "NA,";
					}
				}
				portDfgEdgeMap.clear();

				binOp currBinOp;
				if (node != NULL)
				{
					currBinOp.opcode = HyCUBEInsBinary[node->getFinalIns()];
				}
				else
				{
					currBinOp.opcode = 0;
				}
				//0b111 is used to cut of the output -- maybe switch of the asynchrnous repeater
				currBinOp.outMap[PRED] = 0b111;
				currBinOp.outMap[OP1] = 0b111;
				currBinOp.outMap[OP2] = 0b111;
				currBinOp.outMap[NORTH] = 0b111;
				currBinOp.outMap[EAST] = 0b111;
				currBinOp.outMap[SOUTH] = 0b111;
				currBinOp.outMap[WEST] = 0b111;
				currBinOp.regwen = 0;
				currBinOp.regbypass = 0;
				currBinOp.tregwen = 0;
				currBinOp.constant = 0;
				currBinOp.constantValid = 0;
				currBinOp.npb = 0;

				if (node != NULL)
				{
					if (node->hasConstantVal())
					{
						currBinOp.constant = node->getConstantVal();
						//						currBinOp.outMap[OP2] = node->getConstantVal() >> 27;
						currBinOp.constantValid = 1;
						constantValidMaskMap[j][k] = (constantValidMaskMap[j][k] & (~1) | 1) << 1;
					}
					if (node->getNPB())
					{
						currBinOp.npb = 1;
					}
					LLVM_DEBUG(dbgs() << "CurrNode=" << node->getIdx() << ",placed=" << node->getMappedLoc()->getName() << ",xbarop2=" << currBinOp.outMap[OP2] << "\n");
				}

				for (int l = 0; l < insPortOrder.size(); ++l)
				{
					if (XBarMap[PrevCnode].find(insPortOrder[l]) != XBarMap[PrevCnode].end())
					{
						insFile << currCGRA->getPortName(XBarMap[PrevCnode][insPortOrder[l]]) << ",";
						updateBinOp(&currBinOp, insPortOrder[l], XBarMap[PrevCnode][insPortOrder[l]]);
					}
					else
					{
						insFile << "NA,";
					}
				}

				LLVM_DEBUG(dbgs() << "xbarop2=" << currBinOp.outMap[OP2] << "\n");

				//Print Binary
				binFile << std::setfill('0');
				binFile << std::setw(1) << std::bitset<1>(currBinOp.npb);			//<< ",";
				binFile << std::setw(1) << std::bitset<1>(currBinOp.constantValid); //<< ",";
				binFile << std::setw(27) << std::bitset<27>(currBinOp.constant);	//<< ",";
				binFile << std::setw(5) << std::bitset<5>(currBinOp.opcode);		//<< "," ;
				binFile << std::setw(4) << std::bitset<4>(currBinOp.regwen);		//<< ",";
				binFile << std::bitset<1>(currBinOp.tregwen);						//<< ",";
				binFile << std::setw(4) << std::bitset<4>(currBinOp.regbypass);		//<< ",";
				binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[PRED]);  //<< ",";
				binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP2]);   //<< ",";
				binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP1]);   //<< ",";
				binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[NORTH]); //<< ",";
				binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[WEST]);  //<< ",";
				binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[SOUTH]); //<< ",";
				binFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[EAST]) << std::endl;

				//Print Binary with OpName
				binOpNameFile << std::setfill('0');
				binOpNameFile << std::setw(1) << std::bitset<1>(currBinOp.npb);			  //<< ",";
				binOpNameFile << std::setw(1) << std::bitset<1>(currBinOp.constantValid); //<< ",";
				binOpNameFile << std::setw(27) << std::bitset<27>(currBinOp.constant);	//<< ",";
				binOpNameFile << std::setw(5) << std::bitset<5>(currBinOp.opcode);		  //<< "," ;
				binOpNameFile << std::setw(4) << std::bitset<4>(currBinOp.regwen);		  //<< ",";
				binOpNameFile << std::bitset<1>(currBinOp.tregwen);						  //<< ",";
				binOpNameFile << std::setw(4) << std::bitset<4>(currBinOp.regbypass);	 //<< ",";
				binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[PRED]);  //<< ",";
				binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP2]);   //<< ",";
				binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[OP1]);   //<< ",";
				binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[NORTH]); //<< ",";
				binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[WEST]);  //<< ",";
				binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[SOUTH]); //<< ",";
				binOpNameFile << std::setw(3) << std::bitset<3>(currBinOp.outMap[EAST]) << ",";
				if (node != NULL)
				{
					binOpNameFile << HyCUBEInsStrings[node->getFinalIns()] << std::endl;
				}
				else
				{
					binOpNameFile << "NULL" << std::endl;
				}
			}
		}
		mapFile << std::endl;
		insFile << std::endl;
		binFile << std::endl;
	}

	//Check whether all the parents are accounted for
	for (std::pair<dfgNode *, std::set<dfgNode *>> unit : parentInfo)
	{
		dfgNode *node = unit.first;
		if (node->getAncestors().size() != parentInfo[node].size())
		{
			LLVM_DEBUG(dbgs() << "This node :" << node->getIdx() << "'s parents are not accounted for\n");
			LLVM_DEBUG(dbgs() << "Accounted parents : ");
			for (dfgNode *par : parentInfo[node])
			{
				LLVM_DEBUG(dbgs() << par->getIdx() << ",");
			}
			LLVM_DEBUG(dbgs() << "\n");
		}
		assert(node->getAncestors().size() == parentInfo[node].size());
	}

	//	LLVM_DEBUG(dbgs() << "parentInfo.size() =" << parentInfo.size() << "\n";
	//	LLVM_DEBUG(dbgs() << "NodeList.size() =" << NodeList.size() << "\n";
	//	assert(parentInfo.size()==NodeList.size());

	binFile << std::endl;
	binOpNameFile << std::endl;

	//	for (int j = 0; j < currCGRA->getYdim(); ++j) {
	//		for (int k = 0; k < currCGRA->getXdim(); ++k) {
	//			binFile << "Y=" << j << ",X=" << k << "ContantValidMask=" << std::setw(32) << std::bitset<32>(constantValidMaskMap[j][k]) << std::endl;
	//			binOpNameFile << "Y=" << j << ",X=" << k << "ContantValidMask=" << std::setw(32) << std::bitset<32>(constantValidMaskMap[j][k]) << std::endl;
	//		}
	//	}

	mapFile.close();
	insFile.close();
	binFile.close();
	binOpNameFile.close();
	return 0;
}

int DFG::updateBinOp(binOp *binOpIns, Port outPort, Port inPort)
{

	switch (outPort)
	{
	case R0:
		if (inPort != R0)
		{
			binOpIns->regwen = binOpIns->regwen | 0b1000;
		}
		break;
	case R1:
		if (inPort != R1)
		{
			binOpIns->regwen = binOpIns->regwen | 0b0001;
		}
		break;
	case R2:
		if (inPort != R2)
		{
			binOpIns->regwen = binOpIns->regwen | 0b0100;
		}
		break;
	case R3:
		if (inPort != R3)
		{
			binOpIns->regwen = binOpIns->regwen | 0b0010;
		}
		break;
	case NORTH:
	case EAST:
	case WEST:
	case SOUTH:
	case OP1:
	case OP2:
	case PRED:
		switch (inPort)
		{
		case R0:
			binOpIns->regbypass = binOpIns->regbypass | 0b0100;
		case NORTH:
			binOpIns->outMap[outPort] = 0b011;
			break;
		case R1:
			binOpIns->regbypass = binOpIns->regbypass | 0b0001;
		case EAST:
			binOpIns->outMap[outPort] = 0b000;
			break;
		case R2:
			binOpIns->regbypass = binOpIns->regbypass | 0b0010;
		case WEST:
			binOpIns->outMap[outPort] = 0b010;
			break;
		case R3:
			binOpIns->regbypass = binOpIns->regbypass | 0b1000;
		case SOUTH:
			binOpIns->outMap[outPort] = 0b001;
			break;
		case TILE:
			binOpIns->outMap[outPort] = 0b100;
			break;
		case TREG:
			binOpIns->outMap[outPort] = 0b101;
			break;
		case INV:
			binOpIns->outMap[outPort] = 0b111;
			break;
		default:
			assert(false);
			break;
		}
		break;
		case TREG:
			if (inPort != TREG)
			{
				binOpIns->tregwen = 0b1;
			}
			break;
		default:
			assert(false);
			break;
	}
	return 0;
}

int DFG::printCongestionInfo()
{
	std::map<int, std::vector<dfgNode *>> nodeMapASAPLevels;
	std::map<int, double> regEdgeCountASAPLevels;
	dfgNode *node;
	dfgNode *child;
	int childASAPLevel;
	bool sanity = false;

	for (int i = 0; i < maxASAPLevel; ++i)
	{
		regEdgeCountASAPLevels[i] = 0;
	}

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		nodeMapASAPLevels[node->getASAPnumber()].push_back(node);

		for (int j = 0; j < node->getChildren().size(); ++j)
		{
			child = node->getChildren()[j];
			childASAPLevel = child->getASAPnumber();
			for (int k = node->getASAPnumber() + 1; k < childASAPLevel; ++k)
			{
				regEdgeCountASAPLevels[k]++;
			}
		}
	}

	int MII = (int)std::ceil((double)NodeList.size() / ((double)currCGRA->getXdim() * (double)currCGRA->getYdim()));
	std::map<int, std::map<int, int>> nodeMapEst;
	std::map<int, int> totalNodeCountMapEst;

	std::map<int, std::map<int, double>> edgeMapEst;
	std::map<int, double> totalEdgeCountMapEst;

	int cgraNodePerLevel = currCGRA->getXdim() * currCGRA->getYdim();

	int II = MII;
	int t;
	int T;

	for (int i = 0; i < II; ++i)
	{
		totalNodeCountMapEst[i] = 0;
		nodeMapEst[i][-1] = -1;
	}

	int mapLevel = 0;
	int nodeNumberToBePlaced = 0;
	for (int i = 0; i < maxASAPLevel; ++i)
	{
		LLVM_DEBUG(dbgs() << "printCongestionInfo :: ASAPLevel=" << i << "\n");
		t = mapLevel % II;
		T = mapLevel / II;

		if (totalNodeCountMapEst[t] + nodeMapASAPLevels[i].size() <= cgraNodePerLevel)
		{
			totalNodeCountMapEst[t] += nodeMapASAPLevels[i].size();
			assert(nodeMapEst[t].find(T) == nodeMapEst[t].end());
			nodeMapEst[t][T] = nodeMapASAPLevels[i].size();
			edgeMapEst[t][T] = regEdgeCountASAPLevels[i];
			mapLevel++;
			nodeNumberToBePlaced = 0;
		}
		else
		{
			nodeNumberToBePlaced = totalNodeCountMapEst[t] + nodeMapASAPLevels[i].size() - cgraNodePerLevel;
			LLVM_DEBUG(dbgs() << "first nodeNumberToBePlaced=" << nodeNumberToBePlaced << "\n");
			totalNodeCountMapEst[t] = cgraNodePerLevel;
			assert(nodeMapEst[t].find(T) == nodeMapEst[t].end());
			nodeMapEst[t][T] = cgraNodePerLevel - totalNodeCountMapEst[t];
			edgeMapEst[t][T] = (regEdgeCountASAPLevels[i] * (double)nodeMapEst[t][T]) / (double)nodeMapASAPLevels[i].size();
			assert(nodeNumberToBePlaced > 0);

			while (nodeNumberToBePlaced != 0)
			{
				LLVM_DEBUG(dbgs() << "while nodeNumberToBePlaced=" << nodeNumberToBePlaced << "\n");
				mapLevel++;
				t = mapLevel % II;
				T = mapLevel / II;
				if (totalNodeCountMapEst[t] + nodeNumberToBePlaced <= cgraNodePerLevel)
				{
					totalNodeCountMapEst[t] += nodeNumberToBePlaced;
					assert(nodeMapEst[t].find(T) == nodeMapEst[t].end());
					nodeMapEst[t][T] = nodeNumberToBePlaced;
					edgeMapEst[t][T] = (regEdgeCountASAPLevels[i] * (double)nodeMapEst[t][T]) / (double)nodeMapASAPLevels[i].size();
					mapLevel++;
					nodeNumberToBePlaced = 0;
				}
				else
				{
					nodeNumberToBePlaced = totalNodeCountMapEst[t] + nodeNumberToBePlaced - cgraNodePerLevel;
					LLVM_DEBUG(dbgs() << "while else nodeNumberToBePlaced=" << nodeNumberToBePlaced << "\n");
					totalNodeCountMapEst[t] = cgraNodePerLevel;
					assert(nodeMapEst[t].find(T) == nodeMapEst[t].end());
					nodeMapEst[t][T] = cgraNodePerLevel - totalNodeCountMapEst[t];
					edgeMapEst[t][T] = (regEdgeCountASAPLevels[i] * (double)nodeMapEst[t][T]) / (double)nodeMapASAPLevels[i].size();
				}

				//sanitycheck
				sanity = false;
				for (int i = 0; i < II; ++i)
				{
					assert(totalNodeCountMapEst[i] <= cgraNodePerLevel);
					if (totalNodeCountMapEst[i] < cgraNodePerLevel)
					{
						sanity = true;
					}
				}
				if (!sanity)
				{
					LLVM_DEBUG(dbgs() << "printCongestionInfo is crazy!\n");
					exit(-1);
				}
			}
		}
	}
	LLVM_DEBUG(dbgs() << "printCongestionInfo :: ASAPLevels done.\n");

	std::ofstream outFile;
	std::string outFileName = name + "_congestinfo.txt";
	outFile.open(outFileName.c_str());
	std::map<int, int>::iterator it;

	for (int i = 0; i < II; ++i)
	{
		LLVM_DEBUG(dbgs() << "t=" << std::to_string(i));
		outFile << "t=" << std::to_string(i);

		for (int j = 0; j < nodeMapEst[i].size() - 1; ++j)
		{
			assert(nodeMapEst[i].find(j) != nodeMapEst[i].end());
			LLVM_DEBUG(dbgs() << "," << std::to_string(nodeMapEst[i][j]));
			outFile << "," << std::to_string(nodeMapEst[i][j]);
		}
		LLVM_DEBUG(dbgs() << "\n");
		outFile << std::endl;
	}

	LLVM_DEBUG(dbgs() << "Edges\n");
	outFile << "Edges" << std::endl;

	for (int i = 0; i < II; ++i)
	{
		LLVM_DEBUG(dbgs() << "t=" << std::to_string(i));
		outFile << "t=" << std::to_string(i);

		for (int j = 0; j < edgeMapEst[i].size() - 1; ++j)
		{
			assert(edgeMapEst[i].find(j) != edgeMapEst[i].end());
			LLVM_DEBUG(dbgs() << "," << std::to_string(edgeMapEst[i][j]));
			outFile << "," << std::to_string(edgeMapEst[i][j]);
		}
		LLVM_DEBUG(dbgs() << "\n");
		outFile << std::endl;
	}

	outFile.close();

	LLVM_DEBUG(dbgs() << "printCongestionInfo done\n");
	return 0;
}

int DFG::handleMEMops()
{
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getNode() != NULL)
		{
			if (node->getNode()->mayReadOrWriteMemory())
			{
				node->setIsMemOp(true);
			}
		}
		else
		{
			if ((node->getNameType().compare("LOAD") == 0) ||
					(node->getNameType().compare("STORE") == 0) ||
					(node->getNameType().compare("LOOPSTART") == 0) ||
					(node->getNameType().compare("STORESTART") == 0) ||
					(node->getNameType().compare("LOOPEXIT") == 0) ||
					(node->getNameType().compare("OutLoopSTORE") == 0) ||
					(node->getNameType().compare("OutLoopLOAD") == 0))
			{
				node->setIsMemOp(true);
			}
		}
	}
	return 0;
}

int DFG::getMEMOpsToBePlaced()
{
	int memOpsToBePlaced = 0;
	dfgNode *node;
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getMappedLoc() == NULL)
		{
			if (node->getIsMemOp())
			{
				memOpsToBePlaced++;
			}
		}
	}
	return memOpsToBePlaced;
}

int DFG::getOutLoopMEMOps()
{
	int outloopMEMOps = 0;
	for (dfgNode *node : NodeList)
	{
		if (node->getNameType().compare("OutLoopLOAD") == 0 ||
				node->getNameType().compare("OutLoopSTORE") == 0)
		{
			outloopMEMOps++;
		}
	}
	return outloopMEMOps;
}

DFG::DFG(std::string name, std::map<Loop *, std::string> *lnPtr)
{
	this->name = name;
	this->loopNamesPtr = lnPtr;

	HyCUBEInsStrings[NOP] = "NOP";
	HyCUBEInsBinary[NOP] = 0 | (0b00000);

	HyCUBEInsStrings[ADD] = "ADD";
	HyCUBEInsBinary[ADD] = 0 | (0b00001);

	HyCUBEInsStrings[SUB] = "SUB";
	HyCUBEInsBinary[SUB] = 0 | (0b00010);

	HyCUBEInsStrings[MUL] = "MUL";
	HyCUBEInsBinary[MUL] = 0 | (0b00011);

	HyCUBEInsStrings[SEXT] = "SEXT";
	HyCUBEInsBinary[SEXT] = 0 | (0b00100);

	HyCUBEInsStrings[DIV] = "DIV";
	HyCUBEInsBinary[DIV] = 0 | (0b00101);

	HyCUBEInsStrings[LS] = "LS";
	HyCUBEInsBinary[LS] = 0 | (0b01000);

	HyCUBEInsStrings[RS] = "RS";
	HyCUBEInsBinary[RS] = 0 | (0b01001);

	HyCUBEInsStrings[ARS] = "ARS";
	HyCUBEInsBinary[ARS] = 0 | (0b01010);

	HyCUBEInsStrings[AND] = "AND";
	HyCUBEInsBinary[AND] = 0 | (0b01011);

	HyCUBEInsStrings[OR] = "OR";
	HyCUBEInsBinary[OR] = 0 | (0b01100);

	HyCUBEInsStrings[XOR] = "XOR";
	HyCUBEInsBinary[XOR] = 0 | (0b01101);

	HyCUBEInsStrings[SELECT] = "SELECT";
	HyCUBEInsBinary[SELECT] = 0 | (0b10000);

	HyCUBEInsStrings[CMERGE] = "CMERGE";
	HyCUBEInsBinary[CMERGE] = 0 | (0b10001);

	HyCUBEInsStrings[CMP] = "CMP";
	HyCUBEInsBinary[CMP] = 0 | (0b10010);

	HyCUBEInsStrings[CLT] = "CLT";
	HyCUBEInsBinary[CLT] = 0 | (0b10011);

	HyCUBEInsStrings[BR] = "BR";
	HyCUBEInsBinary[BR] = 0 | (0b10100);

	HyCUBEInsStrings[CGT] = "CGT";
	HyCUBEInsBinary[CGT] = 0 | (0b10101);

	HyCUBEInsStrings[LOADCL] = "LOADCL";
	HyCUBEInsBinary[LOADCL] = 0 | (0b10110);

	HyCUBEInsStrings[MOVCL] = "MOVCL";
	HyCUBEInsBinary[MOVCL] = 0 | (0b10111);

	HyCUBEInsStrings[Hy_LOAD] = "LOAD";
	HyCUBEInsBinary[Hy_LOAD] = 0 | (0b11000);

	HyCUBEInsStrings[Hy_LOADH] = "LOADH";
	HyCUBEInsBinary[Hy_LOADH] = 0 | (0b11001);

	HyCUBEInsStrings[Hy_LOADB] = "LOADB";
	HyCUBEInsBinary[Hy_LOADB] = 0 | (0b11010);

	HyCUBEInsStrings[Hy_STORE] = "STORE";
	HyCUBEInsBinary[Hy_STORE] = 0 | (0b11011);

	HyCUBEInsStrings[Hy_STOREH] = "STOREH";
	HyCUBEInsBinary[Hy_STOREH] = 0 | (0b11100);

	HyCUBEInsStrings[Hy_STOREB] = "STOREB";
	HyCUBEInsBinary[Hy_STOREB] = 0 | (0b11101);

	HyCUBEInsStrings[JUMPL] = "JUMPL";
	HyCUBEInsBinary[JUMPL] = 0 | (0b11110);

	HyCUBEInsStrings[MOVC] = "MOVC";
	HyCUBEInsBinary[MOVC] = 0 | (0b11111);

	// 0b00110 is a free slot between DIV (0b00101) and LS (0b01000).
	// PCMP/NCMP exist in the enum but have no binary entries; avoid stealing.
	HyCUBEInsStrings[MAC] = "MAC";
	HyCUBEInsBinary[MAC] = 0 | (0b00110);

	HyCUBEInsStrings[VADD] = "VADD";
	HyCUBEInsStrings[VSUB] = "VSUB";
	HyCUBEInsStrings[VMUL] = "VMUL";
	HyCUBEInsStrings[VSEXT] = "VSEXT";
	HyCUBEInsStrings[VDIV] = "VDIV";
	HyCUBEInsStrings[VLS] = "VLS";
	HyCUBEInsStrings[VRS] = "VRS";
	HyCUBEInsStrings[VARS] = "VARS";
	HyCUBEInsStrings[VAND] = "VAND";
	HyCUBEInsStrings[VOR] = "VOR";
	HyCUBEInsStrings[VXOR] = "VXOR";
	HyCUBEInsStrings[VCMP] = "VCMP";
	HyCUBEInsStrings[VCLT] = "VCLT";
	HyCUBEInsStrings[VCGT] = "VCGT";
	HyCUBEInsStrings[VSELECT] = "VSELECT";
	HyCUBEInsStrings[VCMERGE] = "VCMERGE";
	HyCUBEInsStrings[VLOAD] = "VLOAD";
	HyCUBEInsStrings[VLOADH] = "VLOADH";
	HyCUBEInsStrings[VLOADB] = "VLOADB";
	HyCUBEInsStrings[VSTORE] = "VSTORE";
	HyCUBEInsStrings[VSTOREH] = "VSTOREH";
	HyCUBEInsStrings[VSTOREB] = "VSTOREB";
	HyCUBEInsStrings[VRED] = "VRED";
	HyCUBEInsStrings[VMAC] = "VMAC";
}

int DFG::nameNodes()
{
	dfgNode *node;

	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];

		if (node->getNode() != NULL)
		{
			switch (node->getNode()->getOpcode())
			{
			case Instruction::Add:
			case Instruction::FAdd:
				node->setFinalIns(ADD);
				break;
			case Instruction::Sub:
			case Instruction::FSub:
				node->setFinalIns(SUB);
				break;
			case Instruction::Mul:
			case Instruction::FMul:
				node->setFinalIns(MUL);
				break;
			case Instruction::UDiv:
			case Instruction::SDiv:
			case Instruction::FDiv:
				node->setFinalIns(DIV);
				break;
			case Instruction::URem:
			case Instruction::SRem:
			case Instruction::FRem:
				LLVM_DEBUG(dbgs() << "REM operations are not implemented\n");
				assert(false);
				break;
			case Instruction::Shl:
				node->setFinalIns(LS);
				break;
			case Instruction::LShr:
				node->setFinalIns(RS);
				break;
			case Instruction::AShr:
				node->setFinalIns(ARS);
				break;
			case Instruction::And:
				node->setFinalIns(AND);
				break;
			case Instruction::Or:
				node->setFinalIns(OR);
				break;
			case Instruction::Xor:
				node->setFinalIns(XOR);
				break;
			case Instruction::Load:
				if (node->getTypeSizeBytes() >= 4)
				{ //TODO : remove this, we dont support 8 byte data structs
					node->setFinalIns(Hy_LOAD);
				}
				else if (node->getTypeSizeBytes() == 2)
				{
					node->setFinalIns(Hy_LOADH);
				}
				else if (node->getTypeSizeBytes() == 1)
				{
					node->setFinalIns(Hy_LOADB);
				}
				else if (node->getNode()->getType()->isPointerTy())
				{ //pointer is a 32 bit address
					node->setFinalIns(Hy_LOAD);
				}
				else
				{
					LLVM_DEBUG(node->getNode()->dump());
					LLVM_DEBUG(dbgs() << "OutLoopLOAD size = " << node->getTypeSizeBytes() << "\n");
					if (node->getNode()->getType() == Type::getDoubleTy(node->getNode()->getContext()))
					{
						node->setFinalIns(Hy_LOAD);
					}
					if (node->getNode()->getType() ==
					    PointerType::get(Type::getDoubleTy(node->getNode()->getContext()), 0))
					{
						node->setFinalIns(Hy_LOAD);
					}
					else
					{
						assert(0);
					}
				}
				break;
			case Instruction::Store:
				if (node->getTypeSizeBytes() >= 4)
				{ //TODO : remove this, we dont support 8 byte data structs
					node->setFinalIns(Hy_STORE);
				}
				else if (node->getTypeSizeBytes() == 2)
				{
					node->setFinalIns(Hy_STOREH);
				}
				else if (node->getTypeSizeBytes() == 1)
				{
					node->setFinalIns(Hy_STOREB);
				}
				else if (node->getNode()->getType()->isPointerTy() || node->getNode()->getOperand(0)->getType()->isPointerTy())
				{ //pointer is a 32 bit address
					node->setFinalIns(Hy_STORE);
				}
				else
				{
					LLVM_DEBUG(node->getNode()->dump());
					LLVM_DEBUG(dbgs() << "TypeSize : " << node->getTypeSizeBytes() << "\n");
					assert(0);
				}
				break;
			case Instruction::GetElementPtr:
				node->setFinalIns(ADD);
				{
					GetElementPtrInst *GEP = cast<GetElementPtrInst>(node->getNode());
					assert(GEPOffsetMap.find(GEP) != GEPOffsetMap.end());

					if (node->getGEPbaseAddr() != -1)
					{
						node->setGEPbaseAddr(node->getGEPbaseAddr() + GEPOffsetMap[GEP]);
					}
					else
					{
						node->setGEPbaseAddr(GEPOffsetMap[GEP]);
					}
					node->setConstantVal(node->getGEPbaseAddr());
				}
				break;

				break;
			case Instruction::Trunc:
			{
				TruncInst *TI = cast<TruncInst>(node->getNode());
				//					node->setConstantVal(TI->getDestTy()->getIntegerBitWidth()/8);
				double destBitWidthDbl = (double)(TI->getDestTy()->getIntegerBitWidth());
				double bitMaskDbl = std::pow(2, destBitWidthDbl) - 1;
				uint32_t bitMask = ((uint32_t)bitMaskDbl);
				node->setConstantVal(bitMask);
				node->setFinalIns(AND);
			}
			break;
			case Instruction::SExt:
			{
				SExtInst *SI = cast<SExtInst>(node->getNode());
				uint32_t srcByteWidth = SI->getSrcTy()->getIntegerBitWidth() / 8;
				uint32_t destByteWidth = SI->getDestTy()->getIntegerBitWidth() / 8;
				uint32_t constOperand = (srcByteWidth << 16) | destByteWidth;
				node->setConstantVal(constOperand);
				node->setFinalIns(SEXT);
			}
			break;
			case Instruction::ZExt:
				node->setFinalIns(OR);
				node->setConstantVal(0);
				break;
			case Instruction::SIToFP:
			case Instruction::FPExt:
			case Instruction::FPTrunc:
			case Instruction::FPToUI:
			case Instruction::FPToSI:
			case Instruction::UIToFP:
			case Instruction::PtrToInt:
			case Instruction::IntToPtr:
			case Instruction::BitCast:
			case Instruction::AddrSpaceCast:
				LLVM_DEBUG(node->getNode()->dump());
				//					assert(0);
				//2019 work
				node->setFinalIns(OR);
				node->setConstantVal(0);
				break;
			case Instruction::PHI:
			case Instruction::Select:
				node->setFinalIns(SELECT);
				break;
			case Instruction::Br:
			{
				BranchInst *BRI = cast<BranchInst>(node->getNode());
				if (BRI->isUnconditional())
				{
					node->setFinalIns(OR);
					node->setConstantVal(1);
				}
				else
				{
					node->setFinalIns(OR);
					node->setConstantVal(0);
				}
			}
			break;
			case Instruction::ICmp:
			{
				LLVM_DEBUG(dbgs() << "NameNodes::Node=" << node->getIdx() << ",CMP=");
				CmpInst *CI = cast<CmpInst>(node->getNode());
				LLVM_DEBUG(CI->dump());
				switch (CI->getPredicate())
				{
				case CmpInst::ICMP_SLT:
				case CmpInst::ICMP_ULT:
					LLVM_DEBUG(dbgs() << "LT\n");
					node->setFinalIns(CLT);
					break;
				case CmpInst::ICMP_SGT:
				case CmpInst::ICMP_UGT:
					LLVM_DEBUG(dbgs() << "GT\n");
					node->setFinalIns(CGT);
					break;
					case CmpInst::ICMP_EQ:
						LLVM_DEBUG(dbgs() << "EQ\n");
						node->setFinalIns(CMP);
						break;
					default:
						DFG_CHECK(false, "nameNodes",
							"unsupported integer comparison predicate " +
							std::to_string(CI->getPredicate()) +
							" for node " + std::to_string(node->getIdx()));
						break;
					}
				}
				break;
			case Instruction::FCmp:
				LLVM_DEBUG(dbgs() << "NameNodes::Node=" << node->getIdx() << ",FCMP\n");
				node->setFinalIns(CMP);
				//2019 work
				//					assert(false);
				break;
				default:
					LLVM_DEBUG(dbgs() << "The Op :" << node->getNode()->getOpcodeName() << " that I thought would not be in the compiled code\n");
					LLVM_DEBUG(node->getNode()->dump());
					DFG_CHECK(false, "nameNodes",
						"unsupported LLVM opcode '" +
						std::string(node->getNode()->getOpcodeName()) +
						"' for node " + std::to_string(node->getIdx()));
					break;
				}
		}
		else
		{
			if (node->getNameType().compare("LOAD") == 0)
			{
				if (node->getTypeSizeBytes() == 4)
				{
					node->setFinalIns(Hy_LOAD);
				}
				else if (node->getTypeSizeBytes() == 2)
				{
					node->setFinalIns(Hy_LOADH);
				}
				else if (node->getTypeSizeBytes() == 1)
				{
					node->setFinalIns(Hy_LOADB);
				}
				else
				{
					assert(0);
				}
			}
			else if (node->getNameType().compare("STORE") == 0)
			{
				if (node->getTypeSizeBytes() == 4)
				{
					node->setFinalIns(Hy_STORE);
				}
				else if (node->getTypeSizeBytes() == 2)
				{
					node->setFinalIns(Hy_STOREH);
				}
				else if (node->getTypeSizeBytes() == 1)
				{
					node->setFinalIns(Hy_STOREB);
				}
				else
				{
					assert(0);
				}
			}
			else if (node->getNameType().compare("NORMAL") == 0)
			{
				node->setFinalIns(ADD);
			}
			else if (node->getNameType().compare("CTRLBrOR") == 0)
			{
				node->setFinalIns(OR);
			}
			else if (node->getNameType().compare("SELECTPHI") == 0)
			{
				node->setFinalIns(SELECT);
			}
			else if (node->getNameType().compare("OutLoopSTORE") == 0)
			{
				if (node->getTypeSizeBytes() == 4)
				{
					node->setFinalIns(Hy_STORE);
				}
				else if (node->getTypeSizeBytes() == 2)
				{
					node->setFinalIns(Hy_STOREH);
				}
				else if (node->getTypeSizeBytes() == 1)
				{
					node->setFinalIns(Hy_STOREB);
				}
				else
				{
					LLVM_DEBUG(OutLoopNodeMapReverse[node]->dump());
					if (OutLoopNodeMapReverse[node]->getType()->isDoubleTy())
					{
						//TODO : to make it compatible with double
						node->setFinalIns(Hy_STORE);
					}
					else
					{
						assert(0);
					}
				}
				node->setConstantVal(node->getoutloopAddr());
			}
			else if (node->getNameType().compare("OutLoopLOAD") == 0)
			{
				if (node->getTypeSizeBytes() == 4)
				{
					node->setFinalIns(Hy_LOAD);
				}
				else if (node->getTypeSizeBytes() == 2)
				{
					node->setFinalIns(Hy_LOADH);
				}
				else if (node->getTypeSizeBytes() == 1)
				{
					node->setFinalIns(Hy_LOADB);
				}
				else
				{
					LLVM_DEBUG(OutLoopNodeMapReverse[node]->dump());
					LLVM_DEBUG(dbgs() << "OutLoopLOAD size = " << node->getTypeSizeBytes() << "\n");
					if (OutLoopNodeMapReverse[node]->getType()->isDoubleTy())
					{
						//TODO : to make it compatible with double
						node->setFinalIns(Hy_LOAD);
					}
					else
					{
						assert(0);
					}
				}
				node->setConstantVal(node->getoutloopAddr());
			}
			else if (node->getNameType().compare("CMERGE") == 0)
			{
				node->setFinalIns(CMERGE);
			}
			else if (node->getNameType().compare("LOOPSTART") == 0)
			{
				//				node->setFinalIns(BR);
				node->setFinalIns(Hy_LOADB);
				node->setConstantVal(MEM_SIZE - 2);
			}
			else if (node->getNameType().compare("LOADSTART") == 0)
			{
				node->setFinalIns(Hy_LOADB);
				node->setConstantVal(MEM_SIZE - 2);
			}
			else if (node->getNameType().compare("STORESTART") == 0)
			{
				node->setFinalIns(Hy_STOREB);
				node->setConstantVal(MEM_SIZE - 2);
			}
			else if (node->getNameType().compare("LOOPEXIT") == 0)
			{
				node->setFinalIns(Hy_STOREB);
				node->setConstantVal(MEM_SIZE/2  - 1);
			}
			else if (node->getNameType().compare("MOVC") == 0)
			{
				node->setFinalIns(MOVC);
			}
			else if (node->getNameType().compare("XORNOT") == 0)
			{
				node->setFinalIns(XOR);
				node->setConstantVal(1);
			}
			else if (node->getNameType().compare("ORZERO") == 0)
			{
				node->setFinalIns(OR);
				node->setConstantVal(0);
			}
				else if (node->getNameType().compare("GEPLEFTSHIFT") == 0)
				{
					node->setFinalIns(LS);
				}
				else if (node->getNameType().compare("GEPSCALE") == 0)
				{
					node->setFinalIns(MUL);
				}
				else if (node->getNameType().compare("GEPADD") == 0)
				{
					node->setFinalIns(ADD);
				}
			else if (node->getNameType().compare("MASKAND") == 0)
			{
				node->setFinalIns(AND);
			}
			else if (node->getNameType().compare("PREDAND") == 0)
			{
				node->setFinalIns(AND);
			}
			else if (node->getNameType().compare("TRIGMERGE") == 0)
			{
				node->setFinalIns(SELECT);
			}
			else
			{
				LLVM_DEBUG(dbgs() << "Unknown custom node \n");
				assert(false);
			}
		}

		hyCUBEInsHist[node->getFinalIns()]++;
	}
	return 0;
}

int DFG::checkSanity()
{
	dfgNode *node;
	//Sanity Check
	for (int i = 0; i < NodeList.size(); ++i)
	{
		node = NodeList[i];
		if (node->getAncestors().size() > 3)
		{
			LLVM_DEBUG(dbgs() << "More than 3 ancestors, NodeIdx=" << node->getIdx());
			if (node->getNode() != NULL)
			{
				LLVM_DEBUG(node->getNode()->dump());
			}
			LLVM_DEBUG(dbgs() << "\n");
		}
		assert(NodeList[i]->getAncestors().size() <= 3);
	}
	return 0;
}

std::string DFG::getArchName(ArchType arch)
{
	switch (arch)
	{
	case RegXbar:
		return "RegXBar";
		break;
	case RegXbarTREG:
		return "RegXbarTREG";
		break;
	case DoubleXBar:
		return "DoubleXBar";
		break;
	case LatchXbar:
		return "LatchXbar";
		break;
	case StdNOC:
		return "StdNOC";
		break;
	case NoNOC:
		return "NoNOC";
		break;
	case ALL2ALL:
		return "ALL2ALL";
		break;
	default:
		return "UnNamed Arch";
		break;
	}
}

bool DFG::MapASAPLevelUnWrapped(int MII, int XDim, int YDim, ArchType arch)
{

	return true;
}
