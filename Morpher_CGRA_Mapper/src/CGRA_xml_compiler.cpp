//============================================================================
// Name        : CGRA_xml_compiler.cpp
// Author      : Manupa Karunaratne
// Version     :
// Copyright   : Your copyright notice
// Description : Hello World in C++, Ansi-style
//============================================================================

#include <iostream>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <dirent.h>

#include <assert.h>
#include <string.h>
// 
#include <morpher/util/util.h>
#include <morpher/dfg/DFG.h>
#include <morpher/arch/CGRA.h>
#include <morpher/mapper/PathFinderMapper.h>
#include <morpher/mapper/SimulatedAnnealingMapper.h>
#include <math.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <cstdlib>

using namespace std;
using namespace CGRAXMLCompile;

namespace
{
std::string format_pf_candidate_topk(int topk)
{
	if (topk == 0)
	{
		return "unbounded";
	}
	if (topk == -1)
	{
		return "auto_fast -> per-node min(32, max(8, ceil(candidates/4)))";
	}
	if (topk == -2)
	{
		return "auto_balanced -> per-node min(64, max(16, ceil(candidates/2)))";
	}
	return std::to_string(topk);
}

std::string format_off_spm_mem_candidate_topk(int topk)
{
	if (topk == 0)
	{
		return "unbounded";
	}
	if (topk == -1)
	{
		return "auto_fast -> per-node min(16, max(4, ceil(candidates/3)))";
	}
	if (topk == -2)
	{
		return "auto_balanced -> per-node min(32, max(8, ceil(candidates/2)))";
	}
	return std::to_string(topk);
}

void print_mapper_arguments(const arguments &args)
{
	std::cout << "Mapper metadata:\n\n";
	std::cout << "[DFG]\n";
	std::cout << "dfg=" << args.dfg_filename << "\n";
	std::cout << "print_dfg=" << args.print_dfg << "\n";
	std::cout << "emit_binary=" << args.emit_binary << "\n\n";

	std::cout << "[Architecture]\n";
	std::cout << "json_arch=" << args.json_file_name << "\n";
	std::cout << "arch_name=" << args.arch_name << "\n";
	std::cout << "xdim=" << args.xdim << "\n";
	std::cout << "ydim=" << args.ydim << "\n";
	std::cout << "petype=" << args.PEType << "\n";
	std::cout << "datapath_number=" << args.ndps << "\n";
	std::cout << "spm_mode=" << args.spm_mode << "\n\n";

	std::cout << "[PathFinder mapper]\n";
	std::cout << "mapping=" << args.mapping_method << "\n";
	std::cout << "ii=" << args.userII << "\n";
	std::cout << "max_II=" << args.max_II << "\n";
	std::cout << "ii_restarts=" << args.ii_restarts << "\n";
	std::cout << "skip_ii_estimation=" << args.skip_ii_estimation << "\n";
	std::cout << "max_iter=" << args.maxiter << "\n";
	std::cout << "backtrack_limit=" << args.backtracklimit << "\n";
	std::cout << "hops=" << args.max_hops << "\n";
	std::cout << "noMutexPaths=" << (args.noMutexPaths ? "true" : "false") << "\n";
	std::cout << "off_spm_mem_candidate_topk=" << format_off_spm_mem_candidate_topk(args.off_spm_mem_candidate_topk) << "\n";
	std::cout << "pf_candidate_topk=" << format_pf_candidate_topk(args.pf_candidate_topk) << "\n";
	std::cout << "pf_guard_mode=" << args.pf_guard_mode << "\n";
	std::cout << "pf_guard_seconds=" << args.pf_guard_seconds << " (0=mode default)\n";
	std::cout << "pf_guard_max_consecutive=" << args.pf_guard_max_consecutive << " (0=mode default)\n";
	std::cout << "pf_frontier_stall_repeats=" << args.pf_frontier_stall_repeats << " (0=disabled)\n";
	std::cout << "pf_frontier_stall_min_mapped=" << args.pf_frontier_stall_min_mapped << "\n";
	std::cout << "pf_util_headroom=" << args.pf_util_headroom << " (0=mode default)\n";
	std::cout << "pf_stagnation_iters=" << args.pf_stagnation_iters << " (0=mode default)\n";
	std::cout << "pf_stagnation_percent=" << args.pf_stagnation_percent << " (0=mode default)\n";
	std::cout << "pf_debug_dumps=" << args.pf_debug_dumps << "\n";
	std::cout << "rut_recovery_seed=" << args.rut_recovery_seed << "\n";
	std::cout << "pf_node_order=" << args.pf_node_order << "\n";
	std::cout << "recmin_policy=" << args.recmin_policy << "\n";
	std::cout << "proximity_sort=" << args.proximity_sort << "\n";
	std::cout << "fanout_route_policy=" << args.fanout_route_policy << "\n\n";

	std::cout << "[SA mapper]\n";
	std::cout << "sa_seed=" << args.sa_seed << "\n";
	std::cout << "sa_restarts=" << args.sa_restarts << "\n";
	std::cout << "sa_moves_per_temp=" << args.sa_moves_per_temp << "\n";
	std::cout << "sa_temp_max=" << args.sa_temp_max << "\n";
	std::cout << "sa_temp_min=" << args.sa_temp_min << "\n";
	std::cout << "sa_candidate_topk=" << args.sa_candidate_topk << "\n";
	std::cout << "sa_stall_temp_steps=" << args.sa_stall_temp_steps << " (0=disabled)\n";
	std::cout << "sa_stall_min_unmapped=" << args.sa_stall_min_unmapped << "\n\n";
	std::cout << std::flush;
}
}

int main(int argn, char *argc[])
{
	//cout << "!!!Hello World!!!" << endl; // prints !!!Hello World!!!

	// if (argn < 7)
	// {
	// 	std::cout << "arguments : <DFG.xml> <peType::\nGENERIC_8REGF,\nHyCUBE_8REGF,\nHyCUBE_4REG,\nN2N_4REGF,\nN2N_8REGF,\nSTDNOC_8REGF,\nSTDNOC_4REGF,\nSTDNOC_4REG,\nSTDNOC_4REGF_1P\nMFU_HyCUBE_4REG\nMFU_HyCUBE_4REGF\nMFU_STDNOC_4REG\nMFU_STDNOC_4REGF> <XYDim> <numberofDPS> <backtracklimit> <initII> <-arch_json> <-noMTpath>\n";
	// }
	// assert(argn >= 7);
	arguments args = parse_arguments(argn,argc);
	print_mapper_arguments(args);
	std::string inputDFG_filename = args.dfg_filename;
	int xdim = args.xdim;
	int ydim = args.ydim;
	string PEType = args.PEType;
	int numberOfDPs = args.ndps;
	string json_file_name = args.json_file_name;
	int initUserII = args.userII;
	int mapping_method = args.mapping_method;
	int max_II = args.max_II;
	int ii_restarts = args.ii_restarts >= 0 ? args.ii_restarts : 0;
	int attempts_per_ii = ii_restarts + 1;
	int sa_restarts = args.sa_restarts >= 0 ? args.sa_restarts : 0;
	int sa_attempts_per_ii = sa_restarts + 1;
	bool skip_ii_estimation = (args.skip_ii_estimation != 0);
	bool print_dfg = (args.print_dfg != 0);
	bool emit_binary = (args.emit_binary != 0);
	const char *print_dfg_env = std::getenv("MORPHER_PRINT_DFG");
	if (print_dfg_env != nullptr && print_dfg_env[0] != '\0')
	{
		print_dfg = (std::string(print_dfg_env) != "0");
	}
	std::string spm_mode = args.spm_mode;
	for (char &ch : spm_mode)
	{
		ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
	}
	if (spm_mode != "on" && spm_mode != "off" && spm_mode != "auto")
	{
		std::cerr << "Warning: unsupported spm_mode '" << args.spm_mode
		          << "'; defaulting to on\n";
		spm_mode = "on";
	}
	std::string arch_name  = args.arch_name;

	
	DFG currDFG;
	currDFG.parseXML(inputDFG_filename);
	if (print_dfg)
	{
		currDFG.printDFG();
	}

	bool isGenericPE;

	//std::cout<<"mapping method:"<<mapping_method<<"\n";

	//std::cout<<"architecture json file:"<<json_file_name<<"\n";

	// CGRA testCGRA(NULL, "testCGRA", 1, ydim, xdim, &currDFG, PEType, numberOfDPs);

	CGRA *testCGRA;

	auto apply_spm_mode = [&](CGRA *target_cgra) -> bool
	{
		const bool has_spm_layout = !target_cgra->Variable2SPM.empty();
		if (spm_mode == "off")
		{
			target_cgra->is_spm_modelled = false;
			return true;
		}
		if (spm_mode == "on")
		{
			if (!has_spm_layout)
			{
				std::cerr << "Error: spm_mode=on requested, but architecture has no SPM modules/data layout.\n";
				return false;
			}
			target_cgra->is_spm_modelled = true;
			return true;
		}

		// auto: infer from architecture content.
		target_cgra->is_spm_modelled = has_spm_layout;
		return true;
	};
	if (!args.use_json)
	{
		testCGRA = new CGRA(NULL, "coreCGRA", 1, ydim, xdim, &currDFG, PEType, numberOfDPs);
	}
	else
	{
		testCGRA = new CGRA(json_file_name, 1,xdim,ydim);
	}

	if (!apply_spm_mode(testCGRA))
	{
		return 1;
	}
	TimeDistInfo tdi = testCGRA->analyzeTimeDist();
	PathFinderMapper *pf_mapper = new PathFinderMapper(inputDFG_filename);
	SAMapper *sa_mapper = new SAMapper(inputDFG_filename);
	sa_mapper->configureSAParams(args.sa_seed,
	                            args.sa_moves_per_temp,
	                            args.sa_temp_max,
	                            args.sa_temp_min,
	                            args.sa_candidate_topk);
	sa_mapper->configureSAStallPolicy(args.sa_stall_temp_steps,
	                                  args.sa_stall_min_unmapped);

	PathFinderMapper * mapper = nullptr;
	if (mapping_method == 0)
	{
		mapper = pf_mapper;
	}
	else if (mapping_method == 1)
	{
		mapper = sa_mapper;
	}
	else if (mapping_method == 2)
	{
		mapper = pf_mapper;
	}
	else
	{
		std::cerr << "Error: invalid mapping method " << mapping_method << " (valid: 0=PathFinder, 1=SA, 2=Hybrid)\n";
		return 1;
	}

		
	pf_mapper->setMaxIter(args.maxiter);
	sa_mapper->setMaxIter(args.maxiter);
	int pfGuardSeconds = 120;
	int pfGuardAbsoluteSeconds = 600;
	int pfGuardMaxConsecutive = 5;
	if (args.pf_guard_mode == "fast")
	{
		pfGuardSeconds = 60;
		pfGuardAbsoluteSeconds = 300;
		pfGuardMaxConsecutive = 3;
	}
	else if (args.pf_guard_mode == "quality")
	{
		pfGuardSeconds = 180;
		pfGuardAbsoluteSeconds = 1200;
		pfGuardMaxConsecutive = 8;
	}
	else if (args.pf_guard_mode == "exhaust")
	{
		pfGuardSeconds = 300;
		pfGuardAbsoluteSeconds = 3600;
		pfGuardMaxConsecutive = 16;
	}
	if (args.pf_guard_seconds > 0)
	{
		pfGuardSeconds = args.pf_guard_seconds;
		pfGuardAbsoluteSeconds = std::max(pfGuardAbsoluteSeconds, pfGuardSeconds * 5);
	}
	if (args.pf_guard_max_consecutive > 0)
	{
		pfGuardMaxConsecutive = args.pf_guard_max_consecutive;
	}
	pf_mapper->setIterationGuardPolicy(pfGuardSeconds, pfGuardAbsoluteSeconds, pfGuardMaxConsecutive);
	sa_mapper->setIterationGuardPolicy(pfGuardSeconds, pfGuardAbsoluteSeconds, pfGuardMaxConsecutive);
	pf_mapper->setFrontierStallPolicy(args.pf_frontier_stall_repeats,
	                                  args.pf_frontier_stall_min_mapped);
	sa_mapper->setFrontierStallPolicy(args.pf_frontier_stall_repeats,
	                                  args.pf_frontier_stall_min_mapped);
	pf_mapper->setRutRecoverySeed(args.rut_recovery_seed);
	sa_mapper->setRutRecoverySeed(args.rut_recovery_seed);
	pf_mapper->setNodeOrderPolicy(args.pf_node_order);
	sa_mapper->setNodeOrderPolicy(args.pf_node_order);
	pf_mapper->setRecMinPolicy(args.recmin_policy);
	sa_mapper->setRecMinPolicy(args.recmin_policy);

	// Fail an II attempt early when PF mapping depth stagnates far from
	// complete; exhaust mode keeps grinding.
	int stagnationIters = 2;
	int stagnationPercent = 40;
	if (args.pf_guard_mode == "quality")
	{
		stagnationIters = 3;
		stagnationPercent = 60;
	}
	else if (args.pf_guard_mode == "exhaust")
	{
		stagnationIters = 0;
	}
	if (args.pf_stagnation_iters > 0)
	{
		stagnationIters = args.pf_stagnation_iters;
	}
	if (args.pf_stagnation_percent > 0)
	{
		stagnationPercent = args.pf_stagnation_percent;
	}
	pf_mapper->setStagnationPolicy(stagnationIters, stagnationPercent);
	sa_mapper->setStagnationPolicy(stagnationIters, stagnationPercent);
	pf_mapper->setDebugDumps(args.pf_debug_dumps != 0);
	sa_mapper->setDebugDumps(args.pf_debug_dumps != 0);

	// Starting-II utilization headroom: at 100% FU occupancy there is no slack
	// for routing through PEs, so PF cannot realistically converge.
	double utilCap = 1.0;
	if (args.pf_util_headroom > 0)
	{
		utilCap = args.pf_util_headroom / 100.0;
	}
	else if (args.pf_guard_mode != "exhaust")
	{
		utilCap = 0.85;
	}

	std::cout << "PF guard policy: mode=" << args.pf_guard_mode
	          << ", idle_seconds=" << pfGuardSeconds
	          << ", absolute_seconds=" << pfGuardAbsoluteSeconds
	          << ", max_consecutive=" << pfGuardMaxConsecutive
	          << ", stagnation_iters=" << stagnationIters
	          << ", stagnation_percent=" << stagnationPercent
	          << ", util_headroom=" << (int)(utilCap * 100) << "%\n";
	std::cout << "MAP begin... \n";
	int resII = 0;
	int recII = 0;
	int resIIHeadroom = 0;
	if (!skip_ii_estimation)
	{
		resII = mapper->getMinimumII(testCGRA, &currDFG);
		resIIHeadroom = (utilCap < 1.0) ? mapper->getMinimumII(testCGRA, &currDFG, utilCap) : resII;
		recII = mapper->getRecMinimumII(&currDFG);
	}
	int II;
	if (skip_ii_estimation)
	{
		II = std::max(1, initUserII);
	}
	else
	{
		II = std::max(recII, resIIHeadroom);
		II = std::max(initUserII, II);
	}

	std::cout << "Initial target II = " << II << " (ResMin II = " << resII
	          << ", headroom-adjusted ResMin II = " << resIIHeadroom
	          << ", RecMin II = " << recII
	          << ", UserGiven II = " << initUserII << ")\n";

	pf_mapper->enableMutexPaths = !args.noMutexPaths;
	sa_mapper->enableMutexPaths = !args.noMutexPaths;
	pf_mapper->enableBackTracking = true;
	sa_mapper->enableBackTracking = true;
	pf_mapper->backTrackLimit = args.backtracklimit;
	sa_mapper->backTrackLimit = args.backtracklimit;
	pf_mapper->proximitySort = args.proximity_sort;
	sa_mapper->proximitySort = args.proximity_sort;
	pf_mapper->setFanoutRoutePolicy(args.fanout_route_policy);
	sa_mapper->setFanoutRoutePolicy(args.fanout_route_policy);
	pf_mapper->offSpmMemCandidateTopK = args.off_spm_mem_candidate_topk;
	sa_mapper->offSpmMemCandidateTopK = args.off_spm_mem_candidate_topk;
	pf_mapper->pfCandidateTopK = args.pf_candidate_topk;
	sa_mapper->pfCandidateTopK = args.pf_candidate_topk;

	//cout << "json_file_name = " << json_file_name << "\n";
	// exit(EXIT_SUCCESS);

	auto start = chrono::steady_clock::now();

	bool mappingSuccess = false;
	bool binaryEmissionFailedAtII = false;
	using AttemptOutcome = PathFinderMapper::AttemptOutcome;
	struct AttemptResult
	{
		bool success = false;
		AttemptOutcome outcome = AttemptOutcome::FAIL_NORMAL;
	};
	auto try_map_at_current_ii = [&](PathFinderMapper *active_mapper,
	                                 bool use_sa_map,
	                                 bool strict_bookkeeping,
	                                 bool emit_binary,
	                                 const std::string &phase_label,
	                                 int phase_attempts) -> AttemptResult
	{
		AttemptResult result;
		phase_attempts = std::max(1, phase_attempts);
		for (int restart = 0; restart < phase_attempts; ++restart)
		{
			DFG tempDFGStorage;
			tempDFGStorage.parseXML(inputDFG_filename);
			if (print_dfg)
			{
				tempDFGStorage.printDFG();
			}
			DFG *activeDFG = &tempDFGStorage;

			CGRA *tempCGRA;
			if (json_file_name.empty())
			{
				tempCGRA = new CGRA(NULL, "coreCGRA", II, ydim, xdim, activeDFG, PEType, numberOfDPs, active_mapper->getcongestedPortsPtr());
			}
			else
			{
				tempCGRA = new CGRA(json_file_name, II, xdim, ydim, active_mapper->getcongestedPortsPtr());
			}
			if (!apply_spm_mode(tempCGRA))
			{
				delete tempCGRA;
				return result;
			}

			tempCGRA->max_hops = args.max_hops;
			active_mapper->setRestartOffset(restart);
			active_mapper->setStrictBookkeeping(strict_bookkeeping);
			active_mapper->resetAttemptOutcome();
			active_mapper->getcongestedPortsPtr()->clear();
			active_mapper->getconflictedPortsPtr()->clear();
			tempCGRA->analyzeTimeDist(tdi);

			// Apply node_pin: blacklist all DPs except those at the pinned PE.
			// Accepts one or more pins as a comma-separated list:
			//   "idx:X:Y" or "idx1:X1:Y1,idx2:X2:Y2,..."
			if (!args.node_pin.empty()) {
				std::string spec = args.node_pin;
				std::vector<std::tuple<int,int,int>> pins;
				size_t pos = 0;
				while (pos < spec.size()) {
					size_t comma = spec.find(',', pos);
					std::string tok = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
					pos = (comma == std::string::npos) ? spec.size() : comma + 1;
					if (tok.empty()) continue;
					std::istringstream ts(tok);
					int pin_idx, pin_x, pin_y;
					char colon;
					if (ts >> pin_idx >> colon >> pin_x >> colon >> pin_y) {
						pins.emplace_back(pin_idx, pin_x, pin_y);
					}
				}
				for (auto &pin : pins) {
					int pin_idx = std::get<0>(pin);
					int pin_x   = std::get<1>(pin);
					int pin_y   = std::get<2>(pin);
					for (DFGNode &dfgNodeRef : activeDFG->nodeList) {
						DFGNode *dfgNode = &dfgNodeRef;
						if (dfgNode->idx != pin_idx) continue;
						for (PE *pe : tempCGRA->getAllPEListDeterministic()) {
							if (pe->X == pin_x && pe->Y == pin_y)
								continue;
							for (Module *submod : pe->subModules) {
								if (FU *fu = dynamic_cast<FU *>(submod)) {
									for (Module *submod2 : fu->subModules) {
										if (DataPath *dp = dynamic_cast<DataPath *>(submod2))
											dfgNode->blacklistDest.insert(dp);
									}
								}
							}
						}
						break;
					}
				}
			}

			bool phaseSuccess = false;
			std::string phaseFailureReason;
			AttemptOutcome attemptOutcome = AttemptOutcome::FAIL_NORMAL;
			try
			{
				if (use_sa_map)
				{
					SAMapper *typed_mapper = static_cast<SAMapper *>(active_mapper);
					phaseSuccess = typed_mapper->SAMap(tempCGRA, activeDFG);
				}
				else
				{
					phaseSuccess = active_mapper->Map(tempCGRA, activeDFG);
				}
			}
			catch (const std::exception &e)
			{
				phaseSuccess = false;
				phaseFailureReason = e.what();
				attemptOutcome = AttemptOutcome::FAIL_EXCEPTION;
				active_mapper->setAttemptOutcome(attemptOutcome);
			}
			catch (...)
			{
				phaseSuccess = false;
				phaseFailureReason = "unknown non-standard exception";
				attemptOutcome = AttemptOutcome::FAIL_EXCEPTION;
				active_mapper->setAttemptOutcome(attemptOutcome);
			}

			active_mapper->congestionInfoFile.close();
			if (!phaseSuccess && attemptOutcome != AttemptOutcome::FAIL_EXCEPTION)
			{
				attemptOutcome = active_mapper->getAttemptOutcome();
				if (attemptOutcome == AttemptOutcome::UNKNOWN)
				{
					attemptOutcome = AttemptOutcome::FAIL_NORMAL;
				}
			}

			if (!phaseSuccess)
			{
				result.outcome = attemptOutcome;
				if (!phaseFailureReason.empty())
				{
					std::cerr << "Warning: mapper attempt failed at II=" << II
					          << " (" << phase_label << "): " << phaseFailureReason << "\n";
				}
				delete tempCGRA;
				if (restart + 1 < phase_attempts)
				{
					std::cout << "Restarting same II = " << II
					          << " (" << phase_label << ", attempt " << (restart + 2) << "/" << phase_attempts << ")\n";
				}
				continue;
			}

			std::cout << "Map Success with II = " << II << "!!!";
			if (phase_label != "strict")
			{
				std::cout << " [" << phase_label << "]";
			}
			std::cout << "\n";
			try
			{
				active_mapper->sanityCheck();
			}
			catch (const std::exception &e)
			{
				std::cerr << "Warning: sanity check failed at II=" << II
				          << " (" << phase_label << "): " << e.what() << "\n";
				result.outcome = AttemptOutcome::FAIL_SANITY;
				delete tempCGRA;
				if (restart + 1 < phase_attempts)
				{
					std::cout << "Restarting same II = " << II
					          << " (" << phase_label << ", attempt " << (restart + 2) << "/" << phase_attempts << ")\n";
				}
				continue;
			}

			tempCGRA->EmitLayoutObservation(
				activeDFG,
				inputDFG_filename + tempCGRA->getCGRAName() + "_layout_observation.json");

			tempCGRA->EmitMappingStats(
				activeDFG, II,
				inputDFG_filename + tempCGRA->getCGRAName() + "_stats.json");

			if (args.layout_solver)
			{
				std::string agiPath = args.agi_subgraph;
				if (agiPath.empty())
				{
					std::string dir = inputDFG_filename;
					auto slash = dir.find_last_of('/');
					dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
					DIR *d = opendir(dir.c_str());
					if (d)
					{
						const std::string suffix = "_AGI_subgraph.xml";
						struct dirent *ent;
						while ((ent = readdir(d)) != nullptr)
						{
							std::string fn = ent->d_name;
							if (fn.size() > suffix.size() &&
								fn.compare(fn.size() - suffix.size(), std::string::npos, suffix) == 0)
							{
								agiPath = dir + "/" + fn;
								break;
							}
						}
						closedir(d);
					}
				}
				CGRA::LayoutSolverOpts opts{true, args.replicate_data != 0};
				std::string decisionPath = inputDFG_filename + tempCGRA->getCGRAName() + "_layout_decision.json";
				bool changed = tempCGRA->SolveLayout(activeDFG, opts, agiPath, decisionPath);
				if (changed)
				{
					active_mapper->UpdateVariableBaseAddr();
					tempCGRA->WriteMemAllocTxt(
						inputDFG_filename + tempCGRA->getCGRAName() + "_mem_alloc.txt");
				}
			}

			if (!emit_binary)
			{
				result.success = true;
				result.outcome = AttemptOutcome::SUCCESS;
				delete tempCGRA;
				return result;
			}

			if (PEType == "HyCUBE_4REG")
			{
				std::cout << "\nPrinting HyCUBE Binary...\n";
				try
				{
					active_mapper->printHyCUBEBinary(tempCGRA);
				}
				catch (const std::runtime_error &e)
				{
					std::cerr << "Warning: binary generation failed at II=" << II
					          << " (" << e.what() << "), retrying same II restart.\n";
					binaryEmissionFailedAtII = true;
					result.outcome = AttemptOutcome::FAIL_BINARY_EMIT;
					delete tempCGRA;
					if (restart + 1 < phase_attempts)
					{
						std::cout << "Restarting same II = " << II
						          << " (" << phase_label << ", attempt " << (restart + 2) << "/" << phase_attempts << ")\n";
					}
					continue;
				}
			}
			result.success = true;
			result.outcome = AttemptOutcome::SUCCESS;
			delete tempCGRA;
			return result;
		}
		return result;
	};

	while (!mappingSuccess)
	{
		binaryEmissionFailedAtII = false;
		if (mapping_method == 0)
		{
			AttemptResult relaxedResult = try_map_at_current_ii(pf_mapper, false, false, emit_binary, "relaxed", attempts_per_ii);
			if (relaxedResult.success)
			{
				mappingSuccess = true;
				break;
			}
			bool runStrict = (relaxedResult.outcome == AttemptOutcome::FAIL_EXCEPTION ||
			                  relaxedResult.outcome == AttemptOutcome::FAIL_SANITY ||
			                  relaxedResult.outcome == AttemptOutcome::FAIL_BINARY_EMIT);
			if (relaxedResult.outcome == AttemptOutcome::FAIL_ITERATION_GUARD)
			{
				std::cout << "Relaxed PF hit iteration guard at II = " << II
				          << "; skipping strict fallback.\n";
			}
			else if (runStrict)
			{
				std::cout << "Relaxed PF failed due to recoverable issue at II = " << II
				          << "; retrying same II with strict bookkeeping fallback.\n";
				AttemptResult strictResult = try_map_at_current_ii(pf_mapper, false, true, emit_binary, "strict-fallback", attempts_per_ii);
				mappingSuccess = strictResult.success;
			}
			else
			{
				std::cout << "Mapping failed at II = " << II
				          << " (relaxed PF); skipping strict fallback.\n";
			}
		}
		else if (mapping_method == 1)
		{
			AttemptResult saResult = try_map_at_current_ii(sa_mapper, true, true, emit_binary, "sa", sa_attempts_per_ii);
			if (!saResult.success && saResult.outcome == AttemptOutcome::FAIL_SA_STALL)
			{
				std::cout << "SA stalled at II = " << II
				          << " after same-II seeded restarts; increasing II.\n";
			}
			mappingSuccess = saResult.success;
		}
		else if (mapping_method == 2)
		{
			AttemptResult relaxedResult = try_map_at_current_ii(pf_mapper, false, false, emit_binary, "hybrid-relaxed", attempts_per_ii);
			if (relaxedResult.success)
			{
				mappingSuccess = true;
				break;
			}

			bool runStrict = (relaxedResult.outcome == AttemptOutcome::FAIL_EXCEPTION ||
			                  relaxedResult.outcome == AttemptOutcome::FAIL_SANITY ||
			                  relaxedResult.outcome == AttemptOutcome::FAIL_BINARY_EMIT);
			if (relaxedResult.outcome == AttemptOutcome::FAIL_ITERATION_GUARD)
			{
				std::cout << "Hybrid: relaxed PF hit iteration guard at II = " << II
				          << "; skipping strict fallback and trying SA.\n";
			}
			else if (runStrict)
			{
				std::cout << "Hybrid: relaxed PF failed due to recoverable issue at II = " << II
				          << "; trying strict PF before SA.\n";
				AttemptResult strictResult = try_map_at_current_ii(pf_mapper, false, true, emit_binary, "hybrid-strict", attempts_per_ii);
				if (strictResult.success)
				{
					mappingSuccess = true;
					break;
				}
			}
			else
			{
				std::cout << "Hybrid: mapping failed at II = " << II
				          << " (relaxed PF); skipping strict and trying SA.\n";
			}
			AttemptResult saResult = try_map_at_current_ii(sa_mapper, true, true, emit_binary, "hybrid-sa", sa_attempts_per_ii);
			if (!saResult.success && saResult.outcome == AttemptOutcome::FAIL_SA_STALL)
			{
				std::cout << "Hybrid: SA stalled at II = " << II
				          << " after same-II seeded restarts.\n";
			}
			mappingSuccess = saResult.success;
		}

		if (mappingSuccess)
		{
			break;
		}

		for (DFGNode &node : currDFG.nodeList)
		{
			assert(node.rootDP == NULL);
		}

		if (II >= max_II)
		{
			std::cout << "### cannot map:  II max has been reached and exiting...\n";
			break;
		}

		II++;

		PathFinderMapper *upperboundMapper = (mapping_method == 1) ? static_cast<PathFinderMapper *>(sa_mapper) : pf_mapper;
		if (!binaryEmissionFailedAtII && II > upperboundMapper->upperboundII)
		{
			std::cout << "upperbound II reached : " << upperboundMapper->upperboundII << "\n";
			std::cout << "Please use the mapping with II = " << upperboundMapper->upperboundFoundBy
			          << ",with Iter = " << upperboundMapper->upperboundIter << "\n";
			break;
		}

		if (binaryEmissionFailedAtII)
		{
			std::cout << "Increasing II to " << II << " after binary generation failure at previous II\n";
		}
		else
		{
			std::cout << "Increasing II to " << II << "\n";
		}
	}

	auto end = chrono::steady_clock::now();
	std::cout << "Elapsed time in seconds: " << chrono::duration_cast<chrono::seconds>(end - start).count() << " sec\n";
	const std::string method_name = (mapping_method == 2) ? "Hybrid" : mapper->getMappingMethodName();
	std::ofstream result_file;
	result_file.open ("result.txt", std::ios_base::app); 
	result_file<< arch_name <<" "<<inputDFG_filename
		<<" method:"<<method_name<<" "<<II<<" "
		<<" status:" << (mappingSuccess ? "success" : "failed") << " "
		<<chrono::duration_cast<chrono::seconds>(end - start).count()<<"\n";
	result_file.close();
	 
	if (!mappingSuccess)
	{
		std::cerr << "Mapping failed: no legal mapping and binary were produced.\n";
		return 1;
	}
	return 0;
}
