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
#include <chrono>
#include <unordered_map>
#include <tuple>

constexpr int kMaxResourceCost = 100000;     // was MRC
constexpr int kUnusedOpPenalty = 1000;        // was UOP
constexpr int kLargeValue = 100000000;        // was LARGE_VALUE

namespace CGRAXMLCompile
{

//struct definitions
class PathFinderMapper;

struct LatPortHash
{
	size_t operator()(const LatPort &p) const
	{
		return std::hash<int>{}(p.first) ^ std::hash<Port *>{}(p.second);
	}
};

inline int stableNodeIdx(const DFGNode *node)
{
	return node ? node->idx : -1;
}

inline std::string stablePortName(Port *port)
{
	return port ? port->getFullName() : "";
}

inline std::string stableDataPathName(DataPath *dp)
{
	return dp ? dp->getFullName() : "";
}

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
		return std::make_tuple(
			cost,
			stableNodeIdx(child),
			stableDataPathName(childDP),
			childDest.first,
			stablePortName(childDest.second),
			startPort.first,
			stablePortName(startPort.second)
		) > std::make_tuple(
			rhs.cost,
			stableNodeIdx(rhs.child),
			stableDataPathName(rhs.childDP),
			rhs.childDest.first,
			stablePortName(rhs.childDest.second),
			rhs.startPort.first,
			stablePortName(rhs.startPort.second)
		);
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
		return std::make_tuple(
			cost,
			src.first,
			stablePortName(src.second),
			dest.first,
			stablePortName(dest.second)
		) > std::make_tuple(
			rhs.cost,
			rhs.src.first,
			stablePortName(rhs.src.second),
			rhs.dest.first,
			stablePortName(rhs.dest.second)
		);
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
		return std::make_tuple(this->cost, stableNodeIdx(parent))
			> std::make_tuple(rhs.cost, stableNodeIdx(rhs.parent));
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
		// `cost` is a destination-level cost. Keep it outside the per-edge
		// routing queues so it is charged exactly once, including for source
		// nodes with no already-mapped parents.
		bestCost = cost + sumBestCosts(unmappedMemNodeCount, hm);
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
		return std::make_tuple(
			bestCost,
			destLat,
			stableNodeIdx(node),
			stableDataPathName(dest)
		) > std::make_tuple(
			rhs.bestCost,
			rhs.destLat,
			stableNodeIdx(rhs.node),
			stableDataPathName(rhs.dest)
		);
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
		FAIL_SA_STALL,
		FAIL_EXCEPTION,
		FAIL_SANITY,
		FAIL_BINARY_EMIT
	};
	enum class FanoutRoutePolicy
	{
		ADAPTIVE = 0,
		BROADCAST,
		CHAINED
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
	// utilCap < 1.0 reserves FU slack for routing: the returned II is the
	// smallest one keeping every op-class below that utilization fraction.
	int getMinimumII(CGRA *cgra, DFG *dfg, double utilCap = 1.0);
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
	// >0: soft destination-cost penalty for placing a LD/ST away from the
	// dominant bank among already-placed same-array accesses (0=off). Prices
	// bank incoherence like congestion instead of forbidding it.
	int bankAffinityCost = 0;
	int offSpmMemCandidateTopK = 0;
	int pfCandidateTopK = 0;
	int pfAstarExpansionBudget = 200000;
	FanoutRoutePolicy fanoutRoutePolicy = FanoutRoutePolicy::ADAPTIVE;

	int upperboundII = 1000000;
	int upperboundIter = -1;
	int upperboundFoundBy = -1;
	std::string mapping_method_name  = "PathFinder";

	// PathFinderMapper methods
	bool Map(CGRA *cgra, DFG *dfg);
	bool LeastCostPathAstar(LatPort start, LatPort end, DataPath *endDP, std::vector<LatPort> &path, int &cost, DFGNode *node, std::map<Port *, std::set<DFGNode *>> &mutexPaths, DFGNode *currNode);

	bool estimateRouting(DFGNode *node, std::priority_queue<dest_with_cost> &estimatedRoutes, DFGNode **failedNode);
	bool isStatefulTregDestinationAvailable(DFGNode *node, DataPath *candidate) const;
	void applySystolicMemorySideConstraint(DFGNode *node, std::vector<DataPath *> &candidateDests) const;
	void addBankAffinityPenalty(DFGNode *node, const std::vector<DataPath *> &candidateDests, std::map<DataPath *, int> &dpPenaltyMap);
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
	std::ofstream congestionInfoFile;

	std::vector<DFGNode *> getLongestDFGPath(DFGNode *src, DFGNode *dest);

	bool canExitCurrPE(LatPort p);
	static bool checkMEMOp(string op);
	void setMaxIter(int m){maxIter = m;}
	void setIterationGuardPolicy(int idleSeconds, int absoluteSeconds, int maxConsecutive)
	{
		progressMaxIdleSecondsInIter = idleSeconds;
		progressMaxAbsoluteSecondsInIter = absoluteSeconds;
		maxConsecutiveIterationGuards = maxConsecutive;
	}
	void setFrontierStallPolicy(int repeats, int minMapped)
	{
		frontierStallRepeats = std::max(0, repeats);
		frontierStallMinMapped = std::max(0, minMapped);
	}
	// Abort an II attempt when, after `iters` PF iterations, the best mapping
	// depth reached so far still leaves more than `percent` of the DFG
	// unmapped. iters <= 0 disables the check.
	void setStagnationPolicy(int iters, int percent)
	{
		stagnationIters = iters;
		stagnationPercent = std::min(100, std::max(0, percent));
	}
	void setDebugDumps(bool enabled)
	{
		dumpDebugFiles = enabled;
	}
	void setRutRecoverySeed(int seed)
	{
		rutRecoverySeed = seed;
	}
	void setNodeOrderPolicy(const std::string &policy)
	{
		prioritizeMemoryOpsFirst = (policy == "mem_first");
	}
	void setFanoutRoutePolicy(const std::string &policy)
	{
		if (policy == "broadcast")
		{
			fanoutRoutePolicy = FanoutRoutePolicy::BROADCAST;
		}
		else if (policy == "chained")
		{
			fanoutRoutePolicy = FanoutRoutePolicy::CHAINED;
		}
		else
		{
			fanoutRoutePolicy = FanoutRoutePolicy::ADAPTIVE;
		}
	}

	bool Check_DFG_CGRA_Compatibility();

	void UpdateVariableBaseAddr();

	void printHyCUBEBinary(CGRA* cgra);
	void printBinFile(const std::vector<InsFormat>& insFArr, std::string fName, CGRA* cgra);
	void setRestartOffset(int offset){restartOffset = offset;}
	void setStrictBookkeeping(bool enable){strictBookkeeping = enable;}
	void setEnforceSystolicMemorySides(bool enable){enforceSystolicMemorySides = enable;}
	void setAttemptOutcome(AttemptOutcome outcome){lastAttemptOutcome = outcome;}
	AttemptOutcome getAttemptOutcome() const { return lastAttemptOutcome; }
	void resetAttemptOutcome(){lastAttemptOutcome = AttemptOutcome::UNKNOWN;}
	void beginIterationProgress(int iteration, size_t mappedCount, size_t unmappedCount);
	void updateAttemptProgress(const char *phase, DFGNode *node, size_t mappedCount, size_t unmappedCount, size_t candidateCount = 0);
	bool iterationGuardHit(const char *phase);
	std::vector<LatPort> getNextPortsCached(LatPort port);
	std::set<Port *> getConflictPortsCached(Port *port);
	void clearAttemptCaches();

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

	void applyNodeOrderPolicy();
	void perturbNodeOrderAfterGuard(DFGNode *guardedNode, int guardStreak);
	void applyFrontierRutPressure(DFGNode *frontierNode, int guardStreak);
	void removeFailedNode(std::stack<DFGNode *> &mappedNodes, std::stack<DFGNode *> &unmappedNodes, DFGNode *failedNode);
	void clearMappedNode(DFGNode *node);
	void pruneInvalidRoutingPorts(DFGNode *node);
	std::map<DataPath *, int> getLatCandDests(const std::vector<DataPath *> &candidateDests, int minlat);
	bool check_parent_violation = true;

	// PathFinderMapper protected
	int getlatMinStartsPHI(const DFGNode *currNode, const std::map<DFGNode *, std::vector<Port *>> &possibleStarts);

	std::map<Port *, std::set<DFGNode *>> congestedPorts;
	std::map<Port *, std::set<DFGNode *>> conflictedPorts;

	int maxIter = 30;
	int restartOffset = 0;
	bool prioritizeMemoryOpsFirst = false;
	bool strictBookkeeping = true;
	bool enforceSystolicMemorySides = false;
	AttemptOutcome lastAttemptOutcome = AttemptOutcome::UNKNOWN;
	int progressIteration = -1;
	const char *progressPhase = "idle";
	DFGNode *progressNode = nullptr;
	size_t progressMappedCount = 0;
	size_t progressUnmappedCount = 0;
	size_t progressCandidateCount = 0;
	size_t progressAstarCalls = 0;
	size_t progressAstarExpanded = 0;
	size_t progressAstarMaxQueue = 0;
	int progressLastHeartbeatSec = -1;
	int progressMaxIdleSecondsInIter = 120;
	int progressMaxAbsoluteSecondsInIter = 600;
	size_t progressBestUnmappedCount = static_cast<size_t>(-1);
	int maxConsecutiveIterationGuards = 5;
	int frontierStallRepeats = 0;
	int frontierStallMinMapped = 0;
	int stagnationIters = 0;
	int stagnationPercent = 50;
	size_t attemptBestUnmappedCount = static_cast<size_t>(-1);
	bool dumpDebugFiles = false;
	int rutRecoverySeed = 1;
	std::chrono::steady_clock::time_point progressIterationStart;
	std::chrono::steady_clock::time_point progressLastImprovement;
	std::unordered_map<LatPort, std::vector<LatPort>, LatPortHash> nextPortsCache;
	std::unordered_map<Port *, std::set<Port *>> conflictPortsCache;

	std::map<int, int> conflictedTimeStepMap;

	std::set<DFGNode *> RecPHIs;

	std::map<BackEdge, std::set<DFGNode *>> RecCycles;
	std::map<BackEdge, std::set<DFGNode *>> RecCyclesLS;
	int getMaxLatencyBE(DFGNode *node, std::map<DataPath *, beParentInfo> &beParentDests, int &downStreamOps);
	std::vector<DataPath *> modifyMaxLatCandDest(std::map<DataPath *, int> candDestIn, DFGNode *node, bool &changed);

	void GetAllSupportedOPs(Module* currmod, unordered_set<string>& supp_ops, unordered_set<string>& supp_pointers);
};

} /* namespace CGRAXMLCompile */

#endif /* PATHFINDERMAPPER_H_ */
