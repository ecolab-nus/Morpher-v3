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
	int mapping_method = 0; // 0: PathFinderMapper, 1: SAMapper (SimulatedAnnealing)
	int max_II = 32;
	int ii_restarts = 1; // number of additional mapper retries at the same II
	std::string spm_mode = "on"; // on | off | auto
	std::string arch_name = "";
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
		.add_option<int>("-m", "--mapping", "mapping method", 0)
		.add_option<int>("-n", "--datapath_number", "datapath_number", 1)
		.add_option<int>("-r", "--max_iter", "max iteration", 30)
		.add_option<int>("-h", "--hops", "hops for hycube", 4)
		.add_option<bool>("-e", "--noMutexPaths", "noMutexPaths", false)
			.add_option<int>("-b", "--backtrack_limit", "back track limit", 0)
				.add_option<int>("", "--max_II", "max II", 32)
				.add_option<int>("", "--ii_restarts", "additional mapper retries per II", 1)
				.add_option<std::string>("", "--spm_mode", "SPM modeling mode: on|off|auto", "on")
				.add_option<std::string>("", "--arch_name", "architecture name", "")
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
	ret.spm_mode = args.get_option<std::string>("--spm_mode");
	ret.use_json = true;
	ret.arch_name = args.get_option<std::string>("--arch_name");
	assert(ret.dfg_filename!= "");
	assert(ret.json_file_name!= "");
	args.print_as_ini(std::cout);

	return ret;

}

#endif /* MAPPER_UTIL_H_ */
