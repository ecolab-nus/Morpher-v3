#include <morpher/util/argparse.h>
#include <morpher/dfg/DFG.h>
#include <morpher/arch/CGRA.h>
#include <morpher/arch/Module.h>
#include <morpher/arch/Port.h>


#include <iostream>
#include <chrono>
#include <math.h>
#include <set>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#pragma once

#ifndef MAPPER_UTIL_H_
#define MAPPER_UTIL_H_


struct arguments
{
	string dfg_filename;
	int xdim = -1;
	int ydim = -1;
	string PEType;
	string json_file_name;
	int userII = 0;
	bool noMutexPaths=false;
	int backtracklimit = 0; // for PathFinderMapper, do not set this for a high number.
	bool use_json = false;
	int ndps = 1;
	int maxiter = 30;  // for PathFinderMapper,
	int max_hops = 4;  // for HyCUBE
	int mapping_method = 0; // 0: PathFinderMapper, 1: SAMapper (SimulatedAnnealing), 2: Hybrid PF+SA
	int max_II = 32;
	int ii_restarts = 1; // number of additional mapper retries at the same II
	int skip_ii_estimation = 0; // 1: skip Res/Rec II estimation and start from user II
	int print_dfg = 0; // 1: print parsed DFG details
	int emit_binary = 1; // 1: emit binary after legal mapping
	int sa_seed = 0; // 0: auto seed
	int sa_restarts = 2; // number of additional SA retries at the same II
	int sa_moves_per_temp = 100;
	float sa_temp_max = 200.0f;
	float sa_temp_min = 10.0f;
	int sa_candidate_topk = 8;
	int off_spm_mem_candidate_topk = 0; // 0=unbounded; cap PF memory candidates when SPM modelling is off
	std::string spm_mode = "on"; // on | off | auto
	std::string arch_name = "";
	bool proximity_sort = true; // sort candidate DPs by Manhattan dist to parent centroid before A* estimation
	std::string fanout_route_policy = "adaptive"; // broadcast | chained | adaptive
	std::string node_pin = ""; // pin a DFG node to a PE: "node_idx:X:Y"
	int layout_solver = 0; // 1: enable post-mapping data layout solver
	int replicate_data = 0; // 1: allow replicating read-only arrays across banks
	std::string agi_subgraph = ""; // path to *_AGI_subgraph.xml (auto-discovered if empty)
};

inline arguments parse_arguments(int argn, char *argc[]){

	

	arguments ret;

	auto args = util::argparser("CGRA mapper argument parser.");
	args.set_program_name("mapper")
        .add_help_option()
        .add_option<int>("-x", "--xdim", "the x dimension of CGRA", -1)
		.add_option<int>("-y", "--ydim", "the x dimension of CGRA", -1)
		.add_option<std::string>("-j", "--json_arch", "architcture file in json", "")
		.add_option<std::string>("-d", "--dfg", "dfg file", "")
		.add_option<std::string>("-t", "--petype", "PE type", "")
		.add_option<int>("-i", "--ii", "initial II", 0)
			.add_option<int>("-m", "--mapping", "mapping method (0=PathFinder, 1=SA, 2=Hybrid)", 0)
			.add_option<int>("-n", "--datapath_number", "datapath_number", 1)
			.add_option<int>("-r", "--max_iter", "max iteration", 30)
			.add_option<int>("-h", "--hops", "hops for hycube", 4)
			.add_option<bool>("-e", "--noMutexPaths", "noMutexPaths", false)
				.add_option<int>("-b", "--backtrack_limit", "back track limit", 0)
					.add_option<int>("", "--max_II", "max II", 32)
					.add_option<int>("", "--ii_restarts", "additional mapper retries per II", 1)
					.add_option<int>("", "--skip_ii_estimation", "skip Res/Rec II estimation", 0)
					.add_option<int>("", "--print_dfg", "print parsed DFG", 0)
					.add_option<int>("", "--emit_binary", "emit binary after legal mapping", 1)
					.add_option<int>("", "--sa_seed", "SA base seed (0=auto)", 0)
					.add_option<int>("", "--sa_restarts", "additional SA retries per II", 2)
					.add_option<int>("", "--sa_moves_per_temp", "SA movements per temperature step", 100)
					.add_option<std::string>("", "--sa_temp_max", "SA maximum temperature", "200")
					.add_option<std::string>("", "--sa_temp_min", "SA minimum temperature", "10")
					.add_option<int>("", "--sa_candidate_topk", "SA top-K candidate DPs sampled stochastically", 8)
					.add_option<int>("", "--off_spm_mem_candidate_topk", "cap PathFinder memory candidates when spm_mode=off (0=unbounded)", 0)
					.add_option<std::string>("", "--spm_mode", "SPM modeling mode: on|off|auto", "on")
					.add_option<std::string>("", "--arch_name", "architecture name", "")
				.add_option<int>("", "--proximity_sort", "sort candidate DPs by proximity to parent centroid (1=on, 0=off)", 1)
				.add_option<std::string>("", "--fanout_route_policy", "fanout route policy: adaptive|broadcast|chained", "adaptive")
				.add_option<std::string>("", "--node_pin", "pin a DFG node: 'node_idx:X:Y'", "")
				.add_option<int>("", "--layout_solver", "enable post-mapping layout solver (1=on, 0=off)", 0)
				.add_option<int>("", "--replicate_data", "allow replicating read-only arrays across banks (1=on, 0=off)", 0)
				.add_option<std::string>("", "--agi_subgraph", "path to the AGI sub-graph XML (auto-discovered when empty)", "")
			        .parse(argn, argc);

	ret.dfg_filename = args.get_option<std::string>("--dfg");
	ret.xdim = args.get_option<int>("--xdim");
	ret.ydim = args.get_option<int>("--ydim");
	ret.PEType = args.get_option<std::string>("--petype");
	ret.json_file_name = args.get_option<std::string>("--json_arch");
	ret.userII = args.get_option<int>("--ii");
	ret.noMutexPaths = args.get_option<bool>("--noMutexPaths");
	ret.backtracklimit = args.get_option<int>("--backtrack_limit");
	ret.ndps =  args.get_option<int>("--datapath_number");
	ret.maxiter = args.get_option<int>("--max_iter");
	ret.max_hops =  args.get_option<int>("--hops");
	ret.mapping_method = args.get_option<int>("--mapping");
	ret.max_II = args.get_option<int>("--max_II");
	ret.ii_restarts = args.get_option<int>("--ii_restarts");
	ret.skip_ii_estimation = args.get_option<int>("--skip_ii_estimation");
	ret.print_dfg = args.get_option<int>("--print_dfg");
	ret.emit_binary = args.get_option<int>("--emit_binary");
	ret.sa_seed = args.get_option<int>("--sa_seed");
	ret.sa_restarts = args.get_option<int>("--sa_restarts");
	ret.sa_moves_per_temp = args.get_option<int>("--sa_moves_per_temp");
	ret.sa_temp_max = std::stof(args.get_option<std::string>("--sa_temp_max"));
	ret.sa_temp_min = std::stof(args.get_option<std::string>("--sa_temp_min"));
	ret.sa_candidate_topk = args.get_option<int>("--sa_candidate_topk");
	ret.off_spm_mem_candidate_topk = args.get_option<int>("--off_spm_mem_candidate_topk");
	ret.spm_mode = args.get_option<std::string>("--spm_mode");
	ret.use_json = true;
	ret.arch_name = args.get_option<std::string>("--arch_name");
	ret.proximity_sort = args.get_option<int>("--proximity_sort") != 0;
	ret.fanout_route_policy = args.get_option<std::string>("--fanout_route_policy");
	if (ret.fanout_route_policy != "adaptive" &&
	    ret.fanout_route_policy != "broadcast" &&
	    ret.fanout_route_policy != "chained")
	{
		std::cerr << "Error: --fanout_route_policy must be one of: adaptive, broadcast, chained.\n";
		exit(EXIT_FAILURE);
	}
	ret.node_pin = args.get_option<std::string>("--node_pin");
	ret.layout_solver = args.get_option<int>("--layout_solver");
	ret.replicate_data = args.get_option<int>("--replicate_data");
	ret.agi_subgraph = args.get_option<std::string>("--agi_subgraph");
	assert(ret.dfg_filename!= "");
	assert(ret.json_file_name!= "");

	return ret;

}

#endif /* MAPPER_UTIL_H_ */
