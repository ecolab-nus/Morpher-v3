/*
 * PathFinderMapper.h
 *
 * Merged from HeuristicMapper.h and PathFinderMapper.h.
 *
 *  Created on: 31 Mar 2018
 *      Author: manupa
 */

#ifndef PATHFINDERMAPPER_H_
#define PATHFINDERMAPPER_H_

#include <morpher/dfg/DFG.h>
#include <morpher/arch/CGRA.h>
#include <morpher/arch/DataPath.h>
#include <morpher/util/debug.h>
#include <queue>
#include <assert.h>
#include <iostream>
#include <fstream>
#include <math.h>
#include <sstream>
#include <string>

constexpr int kMaxResourceCost = 100000;     // was MRC
constexpr int kUnusedOpPenalty = 1000;        // was UOP
constexpr int kLargeValue = 100000000;        // was LARGE_VALUE

namespace CGRAXMLCompile
{

//struct definitions
class PathFinderMapper;

struct dest_child_with_cost
{
	DFGNode *child;
	DataPath* childDP;
	LatPort childDest;
	LatPort startPort;
	int cost;

	dest_child_with_cost(DFGNode *child, DataPath* dp, LatPort childDest, LatPort startPort, int cost) : child(child), childDP(dp), childDest(childDest), startPort(startPort), cost(cost) {}
	bool operator<(const dest_child_with_cost &rhs) const
	{
		return cost > rhs.cost;
	}
};

struct cand_src_with_cost
{
	LatPort src;
	LatPort dest;
	int cost;
	cand_src_with_cost(LatPort src, LatPort dest, int cost) : src(src), dest(dest), cost(cost) {}

	bool operator<(const cand_src_with_cost &rhs) const
	{
		return cost > rhs.cost;
	}
};

struct parent_cand_src_with_cost
{
	DFGNode *parent;
	std::priority_queue<cand_src_with_cost> cswc;
	int cost;
	parent_cand_src_with_cost(DFGNode *parent, std::priority_queue<cand_src_with_cost> cswc) : parent(parent), cswc(cswc)
	{
		cost = cswc.top().cost;
	}

	bool operator<(const parent_cand_src_with_cost &rhs) const
	{
		return this->cost > rhs.cost;
	}
};

struct dest_with_cost
{
	std::priority_queue<parent_cand_src_with_cost> parentStartLocs;
	std::priority_queue<dest_child_with_cost> alreadyMappedChilds;
	int bestCost;
	DataPath *dest;
	int destLat;
	DFGNode *node;
	dest_with_cost() {}
	dest_with_cost(std::priority_queue<parent_cand_src_with_cost> parentStartLocs,
				   std::priority_queue<dest_child_with_cost> alreadyMappedChilds,
				   DataPath *dest, int destLat, DFGNode *node, int cost,
				   int unmappedMemNodeCount, PathFinderMapper *hm) : parentStartLocs(parentStartLocs), alreadyMappedChilds(alreadyMappedChilds), dest(dest), destLat(destLat), node(node)
	{
		bestCost = sumBestCosts(unmappedMemNodeCount, hm);
	}

	int sumBestCosts(int unmappedMemNodeCount, PathFinderMapper *hm)
	{
		int cost = 0;
		std::priority_queue<parent_cand_src_with_cost> parentStartLocsCopy = parentStartLocs;
		while (!parentStartLocsCopy.empty())
		{
			parent_cand_src_with_cost pcswc = parentStartLocsCopy.top();
			parentStartLocsCopy.pop();
			cost += pcswc.cswc.top().cost;
		}

		std::priority_queue<dest_child_with_cost> alreadyMappedChildCopy = alreadyMappedChilds;
		while (!alreadyMappedChildCopy.empty())
		{
			dest_child_with_cost dcwc = alreadyMappedChildCopy.top();
			alreadyMappedChildCopy.pop();
			cost += dcwc.cost;
		}

		if (cost == 0)
		{
			int freePorts = 0;
			for (Port *p : dest->getPE()->outputPorts)
			{
				Module *parent = dest->getPE()->getParent();
				if (parent->getNextPorts(std::make_pair(dest->getPE()->T, p), hm).empty())
					continue;
				if (p->getNode() == NULL)
				{
					freePorts++;
				}
			}
			cost = cost + (15 - freePorts) * kUnusedOpPenalty;

			int primaryCost = 0;
			for (DFGNode *child : node->children)
			{
				for (DFGNode *parentil : child->parents)
				{
					if (parentil->rootDP != NULL && parentil != node)
					{
						PE *parentilPE = parentil->rootDP->getPE();
						PE *destPE = dest->getPE();
						CGRA *cgra = destPE->getCGRA();

						int dt = (destPE->T - parentilPE->T + cgra->get_t_max()) % cgra->get_t_max();
						primaryCost = primaryCost + dt;
					}
				}
			}
			cost = cost + primaryCost * 100;

			int secondaryCost = 0;
			for (DFGNode *child : node->children)
			{
				for (DFGNode *childchild : node->children)
				{
					for (DFGNode *childparent : childchild->parents)
					{
						if (childparent != childchild)
						{
							for (DFGNode *parent : childparent->parents)
							{
								if (parent != node && parent->rootDP != NULL)
								{
								}
							}
						}
					}
				}
			}

			FU *fu = dest->getFU();
			CGRA *cgra = dest->getCGRA();
			int memcost = 0;
			if (fu->supportedOPs.find("LOAD") != fu->supportedOPs.end())
			{
				double memrescost_dbl = (double)unmappedMemNodeCount / (double)cgra->freeMemNodes;
				memrescost_dbl = memrescost_dbl * (double)kMaxResourceCost;
				memcost = (int)memrescost_dbl;
			}

			cost = cost + memcost;

			LOG(ROUTE) << dest->getPE()->getName() << ",cost=" << cost ;
		}
		cost += getDestTportCost();
		return cost;
	}

	int getDestTportCost()
	{
		PE *destPE = dest->getPE();
		FU *destFU = dest->getFU();

		int latency = destFU->supportedOPs[node->op];
		Port *outputPort = dest->getOutputPort(latency);
		return outputPort->getCongCost();
	}

	bool operator<(const dest_with_cost &rhs) const
	{
		return bestCost > rhs.bestCost;
	}
};

struct beParentInfo
{
	DFGNode *beParent;
	int lat;
	int downStreamOps;

	bool dsMEMfound = false;
	int uptoMEMops = -1;
	bool isLDST = false;

	bool operator<(const beParentInfo &other) const
	{
		return this->beParent < other.beParent;
	}
};

struct InsFormat{
	std::string easto;
	std::string southo;
	std::string westo;
	std::string northo;
	std::string alu_i1;
	std::string alu_i2;
	std::string alu_p;

	std::string treg_we;

	std::string east_reg_we;
	std::string south_reg_we;
	std::string west_reg_we;
	std::string north_reg_we;

	std::string east_reg_bypass;
	std::string south_reg_bypass;
	std::string west_reg_bypass;
	std::string north_reg_bypass;

	std::string opcode;
	std::string constant;
	std::string constant_valid;
	std::string negated_predicate;
};


class PathFinderMapper
{
public:
	enum class AttemptOutcome
	{
		UNKNOWN = 0,
		SUCCESS,
		FAIL_NORMAL,
		FAIL_ITERATION_GUARD,
		FAIL_EXCEPTION,
		FAIL_SANITY,
		FAIL_BINARY_EMIT
	};

	PathFinderMapper(std::string fName)
	{
		fNameLog1 = fName;
		mapping_method_name  = "PathFinder";
	}
	virtual ~PathFinderMapper(){};
	CGRA *cgra;
	DFG *dfg;

	// From HeuristicMapper
	int getMinimumII(CGRA *cgra, DFG *dfg);
	int getRecMinimumII(DFG *dfg);
	bool sanityCheck();
	bool checkRecParentViolation(DFGNode *node, LatPort nextPort);
	void printMappingLog();
	void printMappingLog2();
	std::string dumpMappingToStr();
	std::string getMappingMethodName(){return mapping_method_name;}

	bool enableBackTracking = true;
	bool enableMutexPaths = false;
	int backTrackLimit = 4;
	bool proximitySort = true;

	int upperboundII = 1000000;
	int upperboundIter = -1;
	int upperboundFoundBy = -1;
	std::string mapping_method_name  = "PathFinder";

	// PathFinderMapper methods
	bool Map(CGRA *cgra, DFG *dfg);
	bool LeastCostPathAstar(LatPort start, LatPort end, DataPath *endDP, std::vector<LatPort> &path, int &cost, DFGNode *node, std::map<Port *, std::set<DFGNode *>> &mutexPaths, DFGNode *currNode);

	bool estimateRouting(DFGNode *node, std::priority_queue<dest_with_cost> &estimatedRoutes, DFGNode **failedNode);
	int predictiveRoute(DFGNode *node,
						DataPath *dest,
						const std::map<DFGNode *, Port *> routingSourcesIn,
						const std::map<DFGNode *, DataPath *> mappedNodesIn,
						std::map<DFGNode *, Port *> &routingSourcesOut,
						std::map<DFGNode *, DataPath *> &mappedNodesOut);

	bool Route(DFGNode *node, std::priority_queue<dest_with_cost> &estimatedRoutes, DFGNode **failedNode);
	int calculateCost(LatPort src, LatPort next_to_src, LatPort dest);
	void assignPath(DFGNode *src, DFGNode *dest, std::vector<LatPort> path);

	bool updateCongestionCosts(int iter);
	bool clearCurrMapping();
	std::map<Port *, std::set<DFGNode *>> *getcongestedPortsPtr() { return &congestedPorts; }
	std::map<Port *, std::set<DFGNode *>> *getconflictedPortsPtr() { return &conflictedPorts; }

	bool checkConflictedPortCompatibility();
	bool checkRegALUConflicts();
	bool checkDPFree(DataPath *dp, DFGNode *node, int &penalty);

	bool updateConflictedTimeSteps(int timeStep, int conflicts);
	int getTimeStepConflicts(int timeStep);

	void sortBackEdgePriorityASAP();
	void sortBackEdgePriorityALAP();
	std::ofstream congestionInfoFile;

	std::vector<DFGNode *> getLongestDFGPath(DFGNode *src, DFGNode *dest);
	int getFreeMEMPeDist(PE *currPE);

	bool canExitCurrPE(LatPort p);
	static bool checkMEMOp(string op);
	void setMaxIter(int m){maxIter = m;}

	bool Check_DFG_CGRA_Compatibility();

	void UpdateVariableBaseAddr();

	void printHyCUBEBinary(CGRA* cgra);
	void printBinFile(const std::vector<InsFormat>& insFArr, std::string fName, CGRA* cgra);
	void setRestartOffset(int offset){restartOffset = offset;}
	void setStrictBookkeeping(bool enable){strictBookkeeping = enable;}
	void setAttemptOutcome(AttemptOutcome outcome){lastAttemptOutcome = outcome;}
	AttemptOutcome getAttemptOutcome() const { return lastAttemptOutcome; }
	void resetAttemptOutcome(){lastAttemptOutcome = AttemptOutcome::UNKNOWN;}

	int getIndexOfBin(int t, int y, int x){
		return t * cgra->get_y_max() * cgra->get_x_max() + y * cgra->get_x_max() + x;
	}
	std::tuple<int, int, int> getIndexOfBin(int index);

protected:
	// From HeuristicMapper
	int regDiscourageFactor = 10;
	int PETransitionCostFactor = 100;
	int PortTransitionCost = 1;
	int UOPCostFactor = kUnusedOpPenalty;
	int MEMResourceCost = kMaxResourceCost;

	std::string fNameLog1;
	std::string fNameLog2;

	std::ofstream mappingLog;
	std::ofstream mappingLog2;
	std::ofstream mappingLog4;
	std::vector<DFGNode *> sortedNodeList;

	void removeFailedNode(std::stack<DFGNode *> &mappedNodes, std::stack<DFGNode *> &unmappedNodes, DFGNode *failedNode);
	void clearMappedNode(DFGNode *node);
	int getlatMinStarts(const std::map<DFGNode *, std::vector<Port *>> &possibleStarts);
	std::map<DataPath *, int> getLatCandDests(const std::vector<DataPath *> &candidateDests, int minlat);
	bool check_parent_violation = true;

	// PathFinderMapper protected
	int getlatMinStartsPHI(const DFGNode *currNode, const std::map<DFGNode *, std::vector<Port *>> &possibleStarts);

	std::map<Port *, std::set<DFGNode *>> congestedPorts;
	std::map<Port *, std::set<DFGNode *>> conflictedPorts;

	int maxIter = 30;
	int restartOffset = 0;
	bool strictBookkeeping = true;
	AttemptOutcome lastAttemptOutcome = AttemptOutcome::UNKNOWN;

	std::map<int, int> conflictedTimeStepMap;

	std::set<DFGNode *> RecPHIs;

	std::set<DFGNode *> getElders(DFGNode *node);

	std::map<BackEdge, std::set<DFGNode *>> RecCycles;
	std::map<BackEdge, std::set<DFGNode *>> RecCyclesLS;
	int getMaxLatencyBE(DFGNode *node, std::map<DataPath *, beParentInfo> &beParentDests, int &downStreamOps);
	std::vector<DataPath *> modifyMaxLatCandDest(std::map<DataPath *, int> candDestIn, DFGNode *node, bool &changed);

	void GetAllSupportedOPs(Module* currmod, unordered_set<string>& supp_ops, unordered_set<string>& supp_pointers);
};

} /* namespace CGRAXMLCompile */

#endif /* PATHFINDERMAPPER_H_ */
