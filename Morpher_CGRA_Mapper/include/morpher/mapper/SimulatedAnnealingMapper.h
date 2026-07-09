/*
 * SimulatedAnnealingMapper.h
 * Since PathFinderMapper has implemented many functions and other source file also use PathFinderMapper, better to create a mapper which bases
 * on PathFinderMapper
 */

#include <morpher/mapper/PathFinderMapper.h>
#include <morpher/arch/DataPath.h>

#include <string>
#include <random>
#include <algorithm> 
#include <chrono>
#include <limits>
#include <sys/time.h>
#ifndef SAMAPPER_H_
#define SAMAPPER_H_

namespace CGRAXMLCompile
{


using port_util = std::pair<int,int>;
using dfg_data = std::pair<DFGNode*,DFGNode*>; // <src, des>

class SAMapper : public PathFinderMapper
{
public:
		SAMapper(std::string fName) : PathFinderMapper(fName){
			mapping_method_name  = "SA";
		};

		bool SAMap(CGRA *cgra, DFG *dfg);
		void configureSAParams(int seed, int moves_per_temp, float temp_max, float temp_min, int candidate_topk){
			sa_seed = seed;
			movement_in_each_temp = std::max(1, moves_per_temp);
			maximum_temp = std::max(1.0f, temp_max);
			minimim_temp = std::max(0.001f, std::min(temp_min, maximum_temp));
			sa_candidate_topk = std::max(1, candidate_topk);
		}
		void configureSAStallPolicy(int temp_steps, int min_unmapped){
			sa_stall_temp_steps = std::max(0, temp_steps);
			sa_stall_min_unmapped = std::max(1, min_unmapped);
		}
		void prepareRandomForAttempt();

		bool initMap();
	bool Route(DFGNode *node, std::priority_queue<dest_with_cost> &estimatedRoutes, DFGNode **failedNode);

	float inner_map();
	DFGNode* selectDFGNodeToUnmap();
	DFGNode* selectAParentForDFGNode(DFGNode* target_node);
	bool clearNodeMapping(DFGNode * node);
	std::vector<DataPath*> getRandomDPCandidate(DFGNode *node);
	bool SARoute(DFGNode *node, DataPath * candidate);
	int getCost();
	bool restoreMapping(DFGNode *node, 	std::map<DFGNode*, std::pair<DataPath*, int>> & dfg_node_placement, 
										std::map<dfg_data, std::vector<LatPort>> & data_routing_path);
	
	
	int getCongestionNumber(std::stringstream & congestion_detail);
	int getCongestionNumber(){
		std::stringstream ss;
		return getCongestionNumber(ss);
	};

	int getConflictNumber(std::stringstream & congestion_detail);
	int getConflictNumber(){
		std::stringstream ss;
		return getConflictNumber(ss);
	};
	int getPortUsage();
	int getNumberOfUnmappedNodes();
	int checkAnyUnroutedEdge();
	std::string mapStatus();
	bool isCurrMappingValid(){
		int overuse_number = getCongestionNumber();
		int conflict_number = getConflictNumber();
		int unmapped_node_number = getNumberOfUnmappedNodes();
		int unrouted_edge = checkAnyUnroutedEdge();
		if(unmapped_node_number == 0 && conflict_number == 0 && unrouted_edge > 0) assert(false && "some edges are not routed!");
		
		if(unmapped_node_number == 0 && conflict_number == 0  && overuse_number == 0){
			return true;
		}else{
			return false;
		}
	}

	float updateTemperature(float t, float acceptance_rate)
	{
			if (acceptance_rate > 0.96)
			{
					return t * 0.5;
			}
			else if (acceptance_rate > 0.8)
			{
					return t * 0.9;
			}
			else if (acceptance_rate > 0.15)
			{
					return t * 0.98;
			}
			else
			{
					return t * 0.95;
			}
	}

	static inline double getcurrenttime()
	{
			struct timeval t;
			gettimeofday(&t, NULL);
			return t.tv_sec + t.tv_usec * 0.000001;
	}

		bool whetherAcceptNewMapping(float new_cost, float old_cost, float temperature)
		{
				if (new_cost < old_cost)
						return true;

				float probability = exp(-(new_cost - old_cost) / temperature);
				std::uniform_real_distribution<float> probDist(0.0f, 1.0f);
				return probability > probDist(rng_engine);
		}
	//	bool LeastCostPathAstar(Port* start, Port* end, DataPath* endDP, std::vector<Port*>& path, int& cost, DFGNode* node, std::map<Port*,std::set<DFGNode*>>& mutexPaths, DFGNode* currNode);



protected:

	int congest_num;
	int routing_num;

	float maximum_temp = 200;
	float minimim_temp = 10;
	float curr_temp;
	int movement_in_each_temp = 100;
 	float cold_accept_rate = 0.01;
	std::map<DFGNode*, std::pair<DataPath*, int>> dfg_node_placement;
	std::map<dfg_data, std::vector<LatPort>> data_routing_path;
	int maximum_routing_iteration = 3;

		int curr_cost;
		int sa_seed = 0;
		int sa_candidate_topk = 8;
		int sa_stall_temp_steps = 0;
		int sa_stall_min_unmapped = 1;
		unsigned long long rng_base_seed = 0;
		unsigned long long active_rng_seed = 0;
		bool rng_base_seed_initialized = false;
		std::mt19937 rng_engine;

		int backtrack_credit = 3; // this is different from the PathFinder one.



	

	
};

} /* namespace CGRAXMLCompile */

#endif /* SAMAPPER_H_ */
