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
	int sa_seed = 1; // deterministic seed; set 0 for time-based auto seed
	int sa_restarts = 2; // number of additional SA retries at the same II
	int sa_moves_per_temp = 100;
	float sa_temp_max = 200.0f;
	float sa_temp_min = 10.0f;
	int sa_candidate_topk = 8;
	int sa_stall_temp_steps = 0; // 0=disabled; abort SA at current II after this many temp steps without unmapped-count improvement
	int sa_stall_min_unmapped = 1;
	int off_spm_mem_candidate_topk = 0; // 0=unbounded, -1=auto_fast, -2=auto_balanced; cap PF memory candidates when SPM modelling is off
	int pf_candidate_topk = 0; // 0=unbounded, -1=auto_fast, -2=auto_balanced; cap non-memory PF candidates during route estimation
	std::string pf_guard_mode = "balanced"; // fast | balanced | quality | exhaust
	int pf_guard_seconds = 0; // 0=mode default
	int pf_guard_max_consecutive = 0; // 0=mode default
	int pf_frontier_stall_repeats = 0; // 0=disabled; fail PF iteration after repeated same-frontier failures
	int pf_frontier_stall_min_mapped = 0;
	int pf_util_headroom = 0; // starting-II utilization cap in percent; 0=mode default (85, exhaust=100=off)
	int pf_stagnation_iters = 0; // 0=mode default; abort an II when unmapped stays high after this many PF iterations
	int pf_stagnation_percent = 0; // 0=mode default; the "stays high" threshold as percent of DFG nodes
	int pf_debug_dumps = 0; // 1: write per-iteration mapping.csv/routeInfo.log/congestion.info debug dumps
	int rut_recovery_seed = 1; // deterministic seed for PF same-II rut recovery perturbations
	std::string pf_node_order = "default"; // default | mem_first
	std::string recmin_policy = "path"; // retained for CLI compatibility; final-graph path analysis is mandatory
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
					.add_option<int>("", "--sa_seed", "SA base seed (0=auto)", 1)
					.add_option<int>("", "--sa_restarts", "additional SA retries per II", 2)
					.add_option<int>("", "--sa_moves_per_temp", "SA movements per temperature step", 100)
					.add_option<std::string>("", "--sa_temp_max", "SA maximum temperature", "200")
					.add_option<std::string>("", "--sa_temp_min", "SA minimum temperature", "10")
					.add_option<int>("", "--sa_candidate_topk", "SA top-K candidate DPs sampled stochastically", 8)
					.add_option<int>("", "--sa_stall_temp_steps", "abort SA after this many temperature steps without unmapped-count improvement (0=disabled)", 0)
					.add_option<int>("", "--sa_stall_min_unmapped", "only apply SA stall abort when at least this many nodes remain unmapped", 1)
					.add_option<std::string>("", "--off_spm_mem_candidate_topk", "cap PathFinder memory candidates when spm_mode=off (0=unbounded, auto_fast/aggressive, auto_balanced/moderate)", "0")
					.add_option<std::string>("", "--pf_candidate_topk", "cap non-memory PathFinder candidates during route estimation (0=unbounded, auto_fast/aggressive, auto_balanced/moderate)", "0")
					.add_option<std::string>("", "--pf_guard_mode", "PathFinder iteration guard mode: fast|balanced|quality|exhaust", "balanced")
					.add_option<int>("", "--pf_guard_seconds", "PathFinder per-iteration guard seconds (0=mode default)", 0)
					.add_option<int>("", "--pf_guard_max_consecutive", "consecutive guarded PF iterations before failing PF phase (0=mode default)", 0)
					.add_option<int>("", "--pf_frontier_stall_repeats", "fail PF iteration after repeated same-frontier route failures (0=disabled)", 0)
					.add_option<int>("", "--pf_frontier_stall_min_mapped", "only apply PF frontier stall detector after this many mapped nodes", 0)
					.add_option<int>("", "--pf_util_headroom", "starting-II utilization cap in percent (0=mode default: 85, exhaust off; 100=off)", 0)
					.add_option<int>("", "--pf_stagnation_iters", "abort an II when unmapped stays above threshold after this many PF iterations (0=mode default)", 0)
					.add_option<int>("", "--pf_stagnation_percent", "PF stagnation threshold as percent of DFG nodes (0=mode default)", 0)
					.add_option<int>("", "--pf_debug_dumps", "write per-iteration mapping.csv/routeInfo.log/congestion.info dumps (1=on, 0=off)", 0)
					.add_option<int>("", "--rut_recovery_seed", "deterministic seed for same-II rut recovery perturbations", 1)
					.add_option<std::string>("", "--pf_node_order", "PathFinder node ordering policy: default|mem_first", "default")
					.add_option<std::string>("", "--recmin_policy", "Deprecated compatibility option; final-graph path RecMin is always used", "path")
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
	ret.sa_stall_temp_steps = args.get_option<int>("--sa_stall_temp_steps");
	ret.sa_stall_min_unmapped = args.get_option<int>("--sa_stall_min_unmapped");
	if (ret.sa_stall_temp_steps < 0 || ret.sa_stall_min_unmapped < 1)
	{
		std::cerr << "Error: --sa_stall_temp_steps must be >= 0 and --sa_stall_min_unmapped must be >= 1.\n";
		exit(EXIT_FAILURE);
	}
	std::string off_spm_mem_candidate_topk_arg = args.get_option<std::string>("--off_spm_mem_candidate_topk");
	if (off_spm_mem_candidate_topk_arg == "auto_fast")
	{
		ret.off_spm_mem_candidate_topk = -1;
	}
	else if (off_spm_mem_candidate_topk_arg == "auto_balanced")
	{
		ret.off_spm_mem_candidate_topk = -2;
	}
	else
	{
		try
		{
			ret.off_spm_mem_candidate_topk = std::stoi(off_spm_mem_candidate_topk_arg);
		}
		catch (...)
		{
			std::cerr << "Error: --off_spm_mem_candidate_topk must be an integer, auto_fast, or auto_balanced.\n";
			exit(EXIT_FAILURE);
		}
	}
	if (ret.off_spm_mem_candidate_topk < -2)
	{
		std::cerr << "Error: --off_spm_mem_candidate_topk must be >= 0, auto_fast, or auto_balanced.\n";
		exit(EXIT_FAILURE);
	}
	std::string pf_candidate_topk_arg = args.get_option<std::string>("--pf_candidate_topk");
	if (pf_candidate_topk_arg == "auto_fast")
	{
		ret.pf_candidate_topk = -1;
	}
	else if (pf_candidate_topk_arg == "auto_balanced")
	{
		ret.pf_candidate_topk = -2;
	}
	else
	{
		try
		{
			ret.pf_candidate_topk = std::stoi(pf_candidate_topk_arg);
		}
		catch (...)
		{
			std::cerr << "Error: --pf_candidate_topk must be an integer, auto_fast, or auto_balanced.\n";
			exit(EXIT_FAILURE);
		}
	}
	if (ret.pf_candidate_topk < -2)
	{
		std::cerr << "Error: --pf_candidate_topk must be >= 0, auto_fast, or auto_balanced.\n";
		exit(EXIT_FAILURE);
	}
	ret.pf_guard_mode = args.get_option<std::string>("--pf_guard_mode");
	if (ret.pf_guard_mode != "fast" && ret.pf_guard_mode != "balanced" &&
	    ret.pf_guard_mode != "quality" && ret.pf_guard_mode != "exhaust")
	{
		std::cerr << "Error: --pf_guard_mode must be one of: fast, balanced, quality, exhaust.\n";
		exit(EXIT_FAILURE);
	}
	ret.pf_guard_seconds = args.get_option<int>("--pf_guard_seconds");
	ret.pf_guard_max_consecutive = args.get_option<int>("--pf_guard_max_consecutive");
	ret.pf_frontier_stall_repeats = args.get_option<int>("--pf_frontier_stall_repeats");
	ret.pf_frontier_stall_min_mapped = args.get_option<int>("--pf_frontier_stall_min_mapped");
	ret.rut_recovery_seed = args.get_option<int>("--rut_recovery_seed");
	if (ret.pf_guard_seconds < 0)
	{
		std::cerr << "Error: --pf_guard_seconds must be >= 0.\n";
		exit(EXIT_FAILURE);
	}
	if (ret.pf_guard_max_consecutive < 0)
	{
		std::cerr << "Error: --pf_guard_max_consecutive must be >= 0.\n";
		exit(EXIT_FAILURE);
	}
	if (ret.pf_frontier_stall_repeats < 0 || ret.pf_frontier_stall_min_mapped < 0)
	{
		std::cerr << "Error: --pf_frontier_stall_repeats and --pf_frontier_stall_min_mapped must be >= 0.\n";
		exit(EXIT_FAILURE);
	}
	ret.pf_util_headroom = args.get_option<int>("--pf_util_headroom");
	if (ret.pf_util_headroom < 0 || ret.pf_util_headroom > 100)
	{
		std::cerr << "Error: --pf_util_headroom must be in [0, 100].\n";
		exit(EXIT_FAILURE);
	}
	ret.pf_stagnation_iters = args.get_option<int>("--pf_stagnation_iters");
	ret.pf_stagnation_percent = args.get_option<int>("--pf_stagnation_percent");
	if (ret.pf_stagnation_iters < 0 || ret.pf_stagnation_percent < 0 || ret.pf_stagnation_percent > 100)
	{
		std::cerr << "Error: --pf_stagnation_iters must be >= 0 and --pf_stagnation_percent in [0, 100].\n";
		exit(EXIT_FAILURE);
	}
	ret.pf_debug_dumps = args.get_option<int>("--pf_debug_dumps");
	ret.pf_node_order = args.get_option<std::string>("--pf_node_order");
	if (ret.pf_node_order != "default" && ret.pf_node_order != "mem_first")
	{
		std::cerr << "Error: --pf_node_order must be one of: default, mem_first.\n";
		exit(EXIT_FAILURE);
	}
	ret.recmin_policy = args.get_option<std::string>("--recmin_policy");
	if (ret.recmin_policy != "asap" && ret.recmin_policy != "path")
	{
		std::cerr << "Error: --recmin_policy must be one of: asap, path.\n";
		exit(EXIT_FAILURE);
	}
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
