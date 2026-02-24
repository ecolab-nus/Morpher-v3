/*
 * HeuristicMapper.cpp
 *
 *  Created on: 28 Feb 2018
 *      Author: manupa
 */

#include <morpher/mapper/PathFinderMapper.h>
#include <queue>
#include <assert.h>
#include <math.h>
#include <algorithm> // std::reverse
#include <morpher/arch/DataPath.h>
#include <morpher/arch/FU.h>

#include <stack>
#include <functional>
#include <set>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace CGRAXMLCompile
{

} /* namespace CGRAXMLCompile */

template <typename T>
std::set<T> getUnion(const std::set<T> &a, const std::set<T> &b)
{
	std::set<T> result = a;
	result.insert(b.begin(), b.end());
	return result;
}

int CGRAXMLCompile::PathFinderMapper::getMinimumII(CGRA *cgra, DFG *dfg)
{

	std::map<std::string, int> opHist;

	for (DFGNode &node : dfg->nodeList)
	{

		if (opHist.find(node.op) == opHist.end())
		{
			opHist[node.op] = 0;
		}
		opHist[node.op]++;
	}

	std::map<std::string, std::set<DataPath *>> opFUs;

	assert(cgra->get_t_max() > 0);

	for (PE *currPE : cgra->getSpatialPEList(0))
	{
		for (Module *FU_mod : currPE->subModules)
		{
			if (FU *fu = dynamic_cast<FU *>(FU_mod))
			{
				for (std::pair<std::string, int> pair : fu->supportedOPs)
				{
					std::string suppOp = pair.first;
					for (Module *DP_mod : fu->subModules)
					{
						if (DataPath *dp = dynamic_cast<DataPath *>(DP_mod))
						{
							opFUs[suppOp].insert(dp);
						}
					}
				}
			}
		}
	}

	struct opFUstr
	{
		std::string op;
		int count;
		opFUstr(std::string op, int count) : op(op), count(count){};

		bool operator<(const opFUstr &rhs)
		{
			return this->count < rhs.count;
		}
	};

	std::vector<opFUstr> opFUstr_vec;
	for (std::pair<std::string, std::set<DataPath *>> pair : opFUs)
	{
		opFUstr_vec.push_back(opFUstr(pair.first, pair.second.size()));
	}
	std::sort(opFUstr_vec.begin(), opFUstr_vec.end());

	int ii = 1;
	int cummOp = 0;
	std::set<DataPath *> cummDP;
	for (opFUstr a : opFUstr_vec)
	{
		LOG(MAPPING) << "op=" << a.op << ",opcount=" << opHist[a.op] << ",dpcount=" << a.count;
		int new_ratio = (opHist[a.op] + a.count - 1) / a.count; // int ceil
		LOG(MAPPING) << ",ratio=" << new_ratio;
		ii = std::max(ii, new_ratio);

		cummOp += opHist[a.op];
		for (DataPath *dp : opFUs[a.op])
		{
			cummDP.insert(dp);
		}
		LOG(MAPPING) << ",cummOp=" << cummOp << ",cummDP=" << cummDP.size();
		int cumm_ratio = (cummOp + cummDP.size() - 1) / cummDP.size();
		LOG(MAPPING) << ",ratio=" << cumm_ratio << "\n";
		ii = std::max(ii, cumm_ratio);
	}
	LOG(MAPPING) << "Min II = " << ii << "\n";
	return ii;
}

void CGRAXMLCompile::PathFinderMapper::printMappingLog()
{

	struct util
	{
		void static repeatedPush(std::stringstream &ss, std::string pushStr, int count)
		{
			for (int i = 0; i < count; ++i)
			{
				ss << pushStr;
			}
		}
	};

	vector<string> lineHeaders;

	vector<PE *> zeroth_spatialPEs = cgra->getSpatialPEList(0);
	std::stringstream peHeader;
	std::stringstream fuHeader;
	std::stringstream dpHeader;
	for (PE *pe : zeroth_spatialPEs)
	{
		peHeader << pe->getName();

		int fuCount = 0;
		int dpCount = 0;
		for (Module *mod : pe->subModules)
		{
			if (FU *fu = dynamic_cast<FU *>(mod))
			{
				fuCount++;
				for (Module *mod : fu->subModules)
				{
					if (DataPath *dp = dynamic_cast<DataPath *>(mod))
					{
						dpCount++;
					}
				}
			}
		}

		int inputPortCount = pe->inputPorts.size();
		int outputPortCount = pe->outputPorts.size();
		int regConPortCount = pe->getRegConPorts().size()*2;
		int totalColumns = dpCount + inputPortCount + outputPortCount + regConPortCount;

		for (Module *mod : pe->subModules)
		{
			if (FU *fu = dynamic_cast<FU *>(mod))
			{
				fuHeader << fu->getName() << ",";
				for (Module *mod : fu->subModules)
				{
					if (DataPath *dp = dynamic_cast<DataPath *>(mod))
					{
						dpHeader << dp->getName() << ",";
					}
				}
			}
		}

		for (Port *ip : pe->inputPorts)
		{
			dpHeader << ip->getName() << ",";
		}

		for (Port *op : pe->outputPorts)
		{
			dpHeader << op->getName() << ",";
		}

		for (pair<Port*,Port*> pp : pe->getRegConPorts())
		{
			Port* ri = pp.first;
			Port* ro = pp.second;

			dpHeader << ri->getName() << ",";
			dpHeader << ro->getName() << ",";
		}

		util::repeatedPush(peHeader, ",", totalColumns);
		util::repeatedPush(fuHeader, ",", totalColumns - fuCount);
	}

	mappingLog << "," << peHeader.str() << "\n";
	mappingLog << "," << fuHeader.str() << "\n";
	mappingLog << "," << dpHeader.str() << "\n";

	std::map<int, std::vector<std::vector<std::string>>> lineMatrix;

	for (int t = 0; t < cgra->get_t_max(); ++t)
	{

		vector<PE *> spatialPEs = cgra->getSpatialPEList(t);
		for (PE *pe : spatialPEs)
		{

			std::stringstream peHeader;
			std::stringstream fuHeader;
			std::stringstream dpHeader;
			std::stringstream dpOp;

			peHeader << "T=" << t << "," << pe->getName();

			int fuCount = 0;
			int dpCount = 0;
			for (Module *mod : pe->subModules)
			{
				if (FU *fu = dynamic_cast<FU *>(mod))
				{
					fuCount++;
					for (Module *mod : fu->subModules)
					{
						if (DataPath *dp = dynamic_cast<DataPath *>(mod))
						{
							dpCount++;
						}
					}
				}
			}

			int inputPortCount = pe->inputPorts.size();
			int outputPortCount = pe->outputPorts.size();
			int regConPortCount = pe->getRegConPorts().size()*2;
			int totalColumns = dpCount + inputPortCount + outputPortCount + regConPortCount;

			for (Module *mod : pe->subModules)
			{
				if (FU *fu = dynamic_cast<FU *>(mod))
				{
					fuHeader << fu->getName() << ",";
					for (Module *mod : fu->subModules)
					{
						if (DataPath *dp = dynamic_cast<DataPath *>(mod))
						{
							dpHeader << dp->getName() << ",";
							if (dp->getMappedNode())
							{
								dpOp << dp->getMappedNode()->idx << ":" << dp->getMappedNode()->op;
								if (dp->getMappedNode()->hasConst)
								{
									dpOp << "+C";
								}
								dpOp << "(";
								for (DFGNode *parent : dp->getMappedNode()->parents)
								{
									dpOp << parent->idx << "-";
								}
								dpOp << "|";
								for (DFGNode *child : dp->getMappedNode()->children)
								{
									dpOp << child->idx << "-";
								}
								dpOp << ")";
								dpOp << ",";
							}
							else
							{
								dpOp << "---,";
							}
						}
					}
				}
			}

			for (Port *ip : pe->inputPorts)
			{
				dpHeader << ip->getName() << ",";
				if (ip->getNode())
				{
					dpOp << ip->getNode()->idx /*<< ":" << ip.getNode()->op */ << ",";
				}
				else
				{
					dpOp << "---,";
				}
			}

			for (Port *op : pe->outputPorts)
			{
				dpHeader << op->getName() << ",";
				if (op->getNode())
				{
					dpOp << op->getNode()->idx /*<< ":" << op.getNode()->op */ << ",";
				}
				else
				{
					dpOp << "---,";
				}
			}

			for (pair<Port*,Port*> pp : pe->getRegConPorts())
			{
				Port* ri = pp.first;
				Port* ro = pp.second;

				dpHeader << ri->getName() << ",";
				if (ri->getNode())
				{
					dpOp << ri->getNode()->idx /*<< ":" << op.getNode()->op */ << ",";
				}
				else
				{
					dpOp << "---,";
				}

				dpHeader << ro->getName() << ",";
				if (ro->getNode())
				{
					dpOp << ro->getNode()->idx /*<< ":" << op.getNode()->op */ << ",";
				}
				else
				{
					dpOp << "---,";
				}

			}

			std::vector<string> lineWord;
			lineWord.push_back(dpOp.str());

			lineMatrix[t].push_back(lineWord);
		}
	}

	int lineCount = 1;

	//print line matrix
	for (int t = 0; t < cgra->get_t_max(); ++t)
	{
		mappingLog << "T=" << t << ",";
		for (vector<string> linewords : lineMatrix[t])
		{
			for (int l = 0; l < lineCount; ++l)
			{
				mappingLog << linewords[l];
			}
		}
		mappingLog << "\n";
	}
	mappingLog << "************************************\n";
}

void CGRAXMLCompile::PathFinderMapper::printMappingLog2()
{
	mappingLog2 << "--------------------------------------------------------\n";
	for (DFGNode &node : dfg->nodeList)
	{
		if (node.rootDP != NULL)
		{
			mappingLog2 << "node=" << node.idx << ",mapped=" << node.rootDP->getPE()->getName() << "\n";
			for (DFGNode *parent : node.parents)
			{
				if (parent->rootDP != NULL)
				{
					mappingLog2 << "\t"
								<< "parent:" << parent->idx << "\n";
					for (std::pair<Port *, int> pair : parent->routingPorts)
					{
						Port *p = pair.first;
						if (pair.second == node.idx)
						{
							mappingLog2 << "\t\t" << p->getFullName() << "\n";
						}
					}
				}
			}
		}
	}
	mappingLog2 << "--------------------------------------------------------\n";
}

bool CGRAXMLCompile::PathFinderMapper::sanityCheck()
{
	std::map<Port *, std::set<DFGNode *>> mapInfo;

	for (DFGNode *node : this->sortedNodeList)
	{
		assert(node->rootDP != NULL);
		assert(!node->routingPorts.empty());
		for (std::pair<Port *, int> pair : node->routingPorts)
		{
			mapInfo[pair.first].insert(node);
		}
	}

	for (std::pair<Port *, std::set<DFGNode *>> pair : mapInfo)
	{
		Port *p = pair.first;

		if (pair.second.size() > 1)
		{
			std::cout << p->getFullName() << ":";
			for (DFGNode *node2 : pair.second)
			{
				std::cout << node2->idx << "(" << node2->BB << "),";
			}
			std::cout << "\n";
		}
	}
	return true;
}

void CGRAXMLCompile::PathFinderMapper::clearMappedNode(DFGNode *node)
{
	if (strictBookkeeping)
	{
		node->clear(this->dfg);
	}
	else
	{
		node->relaxedClear(this->dfg);
	}
}

void CGRAXMLCompile::PathFinderMapper::removeFailedNode(std::stack<DFGNode *> &mappedNodes,
														   std::stack<DFGNode *> &unmappedNodes, DFGNode *failedNode)
{

	DFGNode *fNode = failedNode;

	std::stack<DFGNode *> temp;
	DFGNode *curr = NULL;

	while (curr != fNode)
	{
		assert(!mappedNodes.empty());
		curr = mappedNodes.top();
		mappedNodes.pop();

		if (curr != fNode)
		{
			temp.push(curr);
		}
	}
	assert(curr == fNode);

	unmappedNodes.push(curr);
	while (!temp.empty())
	{
		curr = temp.top();
		temp.pop();
		mappedNodes.push(curr);
	}
}

bool CGRAXMLCompile::PathFinderMapper::checkRecParentViolation(DFGNode *node,
															  LatPort nextPort)
{
	if(!check_parent_violation) { return false;}
	for (DFGNode *recParent : node->recParents)
	{
		assert(recParent->rootDP != NULL); //should be mapped
		DataPath *recParentDP = recParent->rootDP;
		if (nextPort.first > recParentDP->getLat() + this->cgra->get_t_max())
		{
			return true;
		}else{
			LOG(ROUTE)<<" satisfy data dependency: "<< node->idx <<","
			<< nextPort.first<<" -> recparent:"<<recParent->idx<<","<<recParentDP->getLat()<<"\n";
		}
	}
	return false;
}

int CGRAXMLCompile::PathFinderMapper::getlatMinStarts(
	const std::map<DFGNode *, std::vector<Port *>> &possibleStarts)
{

	int min;
	std::map<DFGNode *, int> minLat;

	for (std::pair<DFGNode *, std::vector<Port *>> pair : possibleStarts)
	{
		int latm = 100000000;

		for (Port *p : pair.second)
		{
			if (p->getLat() < latm)
			{
				latm = p->getLat();
			}
		}
		assert(latm != 100000000);
		minLat[pair.first] = latm;
	}

	int max = 0;
	for (std::pair<DFGNode *, int> pair : minLat)
	{
		if (max < pair.second)
		{
			max = pair.second;
		}
	}

	return max;
}

//Previously, calculate the minial latency of its parent.
// Now calculate the minimal latency whose remainder should be equal to II of the candidate PE.
std::map<CGRAXMLCompile::DataPath *, int> CGRAXMLCompile::PathFinderMapper::getLatCandDests(
	const std::vector<DataPath *> &candidateDests, int minLat)
{

	std::map<CGRAXMLCompile::DataPath *, int> res;

	for (DataPath *dp : candidateDests)
	{
		PE *pe = dp->getPE();
		CGRA *cgra = dp->getCGRA();
		int ii = cgra->get_t_max();
		int t = pe->T;
		int minLatmodii = minLat % ii;

		int lat = minLat;
		if (minLatmodii > t)
		{
			lat += ii + (t - minLatmodii);
		}
		else
		{
			lat += t - minLatmodii;
		}

		if (lat % ii != t)
		{
			std::cout << "ii = " << ii << "\n";
			std::cout << "minLat = " << minLat << "\n";
			std::cout << "t = " << t << "\n";
			std::cout << "minLatmodii = " << minLatmodii << "\n";
			std::cout << "lat = " << lat << "\n";
		}

		assert(lat % ii == t);
		res[dp] = lat;
	}
	return res;
}


int CGRAXMLCompile::PathFinderMapper::getRecMinimumII(DFG *dfg)
{
	int recMinII = 0;
	int number_of_backedges = 0;
	int recDistance = 0;
	for (DFGNode &node : dfg->nodeList)
	{

		for (DFGNode *child : node.children)
		{

			if (child->ASAP <= node.ASAP)
			{
				LOG(MAPPING) << "Backedge Parent: " << node.idx << ", Child: " << child->idx <<"\n";

				LOG(MAPPING) << "Parent ASAP: " << node.ASAP << ", Child ASAP: " << child->ASAP <<"\n";

				LOG(MAPPING) << "Parent ALAP: " << node.ALAP << ", Child ALAP: " << child->ALAP <<"\n";
				number_of_backedges = number_of_backedges+ 1;
				recDistance = node.ASAP - child->ASAP + 1;
				assert(recDistance>=0);
				if(recDistance > recMinII){
					recMinII = recDistance;
				}

			}
		}
	}


	LOG(MAPPING) << "Number of Backedges: " << number_of_backedges <<"\n";
	LOG(MAPPING) << "Recurrence Minimum II: " << recMinII <<"\n";
	return recMinII;

}


std::string CGRAXMLCompile::PathFinderMapper::dumpMappingToStr(){
	std::vector< std::set<DFGNode *>> latency_to_node_vector; // the index represend the latency
	for(int i = 0; i < 1000; i++){
		std::set<DFGNode *> temp;
		latency_to_node_vector.push_back( temp);
	}
	std::set<DFGNode *> unmapped_dfg_nodes;
	std::map<DFGNode *, int> node_latency;


	//get the latency of each operation
	int max_latency = 0;
	for(auto node: sortedNodeList){
		if(node->rootDP == NULL){
			unmapped_dfg_nodes.insert(node);
			continue;
		}
		int lat = node->rootDP->getLat();
		latency_to_node_vector[lat].insert(node);
		node_latency.emplace(node, lat);
		max_latency = max_latency>lat ? max_latency:lat;
	}

	//verify that the latency satisfies the data dependency
	for(auto& [node, lat]: node_latency){
		for(auto parent: node->parents){
			if (parent->childrenOPType[node] == "PS")
			{
				continue;
			}
			if(parent->childNextIter[node] == 0){
				if(node_latency.find(parent)!= node_latency.end()){
					assert(node_latency[parent] <  lat);
				}
			}else if (parent->childNextIter[node] == 1){
				if(node_latency.find(parent)!= node_latency.end()){
					assert(node_latency[parent] <  lat + this->cgra->get_t_max());
				}
			}else {
				throw std::runtime_error("printMappingLog: unexpected parent->childNextIter value " + std::to_string(parent->childNextIter[node]) + " for node " + std::to_string(node->idx));
			}

		}

		for(auto child: node->children){
			if (node->childrenOPType[child] == "PS")
			{
				continue;
			}
			if(node->childNextIter[child] == 0){
				if(node_latency.find(child)!= node_latency.end()){
					assert(node_latency[child] >  lat);
				}
			}else if (node->childNextIter[child] == 1){
				if(node_latency.find(child)!= node_latency.end()){
					assert(node_latency[child]  + this->cgra->get_t_max()>  lat );
				}
			}else {
				throw std::runtime_error("printMappingLog: unexpected node->childNextIter value " + std::to_string(node->childNextIter[child]) + " for child node " + std::to_string(child->idx));
			}

		}
	}

	//print the mapping
	std::stringstream ss;

	for(int lat = 0;  lat <= max_latency ; lat++ ){
		ss<<"cycle "<<lat <<":\n\t";
		for(auto node: latency_to_node_vector[lat]){
			ss<<"("<<node->rootDP->getPE()->getPosition_X()<<", "<<node->rootDP->getPE()->getPosition_Y()<<")->"
			 << node->idx <<"  ";
		}
		ss<<"\n";
	}

	ss<<"unmapped node: \n";
	for(auto node: unmapped_dfg_nodes){
		ss<<"\t"<<node->idx;

		ss<<"\tparent: ";
		for(auto parent: node->parents){
			ss<<parent->idx<<", ";
		}

		ss<<"\tchild: ";
		for(auto child: node->children){
			ss<<child->idx<<", ";
		}

		ss<<"\trec_parent: ";
		for(auto rec_parent: node->recParents){
			ss<<rec_parent->idx<<", ";
		}
		ss<<"\n";
	}

	return ss.str();

}
