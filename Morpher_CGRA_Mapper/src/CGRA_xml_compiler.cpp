//============================================================================
// Name        : CGRA_xml_compiler.cpp
// Author      : Manupa Karunaratne
// Version     :
// Copyright   : Your copyright notice
// Description : Hello World in C++, Ansi-style
//============================================================================

#include <iostream>
#include <chrono>
#include <stdexcept>

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

using namespace std;
using namespace CGRAXMLCompile;


int main(int argn, char *argc[])
{
	//cout << "!!!Hello World!!!" << endl; // prints !!!Hello World!!!

	// if (argn < 7)
	// {
	// 	std::cout << "arguments : <DFG.xml> <peType::\nGENERIC_8REGF,\nHyCUBE_8REGF,\nHyCUBE_4REG,\nN2N_4REGF,\nN2N_8REGF,\nSTDNOC_8REGF,\nSTDNOC_4REGF,\nSTDNOC_4REG,\nSTDNOC_4REGF_1P\nMFU_HyCUBE_4REG\nMFU_HyCUBE_4REGF\nMFU_STDNOC_4REG\nMFU_STDNOC_4REGF> <XYDim> <numberofDPS> <backtracklimit> <initII> <-arch_json> <-noMTpath>\n";
	// }
	// assert(argn >= 7);
	std::cout<<"Mapper arguments:\n"<<std::flush;
	arguments args = parse_arguments(argn,argc);
	std::cout<<"\n";
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
	bool use_relaxed_heuristic = mapping_method == 0;
	std::string arch_name  = args.arch_name;

	
	DFG currDFG;
	currDFG.parseXML(inputDFG_filename);
	currDFG.printDFG();

	bool isGenericPE;

	//std::cout<<"mapping method:"<<mapping_method<<"\n";

	//std::cout<<"architecture json file:"<<json_file_name<<"\n";

	// CGRA testCGRA(NULL, "testCGRA", 1, ydim, xdim, &currDFG, PEType, numberOfDPs);

	CGRA *testCGRA;
	if (!args.use_json)
	{
		testCGRA = new CGRA(NULL, "coreCGRA", 1, ydim, xdim, &currDFG, PEType, numberOfDPs);
	}
	else
	{
		testCGRA = new CGRA(json_file_name, 1,xdim,ydim);
	}

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
	if (!apply_spm_mode(testCGRA))
	{
		return 1;
	}

	

	TimeDistInfo tdi = testCGRA->analyzeTimeDist();
	PathFinderMapper * mapper;
	if (mapping_method == 0){
		mapper = new PathFinderMapper(inputDFG_filename);
	}else if(mapping_method  == 1){
		mapper = new SAMapper(inputDFG_filename);
	}else{
		std::cerr << "Error: invalid mapping method " << mapping_method << " (valid: 0=PathFinder, 1=SA)\n";
		return 1;
	}

	
	mapper->setMaxIter(args.maxiter);
	std::cout << "MAP begin... \n";
	int resII = mapper->getMinimumII(testCGRA, &currDFG);
	int recII  = mapper->getRecMinimumII(&currDFG);
	// int recII = 0; // for SA initial mapping test.
//	std::cout << "Res Minimum II = " << resII << ", ";
//	std::cout << "Rec Minimum II = " << recII << ", ";
//	std::cout << "Init User II = " << initUserII << "\n";
	int II = std::max(recII, resII);

	II = std::max(initUserII, II);

	std::cout << "Initial target II = " << II << " (ResMin II = "<<resII<<" , RecMin II = "<<recII<<", UserGiven II = "<<initUserII<<" )\n";

	mapper->enableMutexPaths = true;
	if (args.noMutexPaths)
	{
		mapper->enableMutexPaths = false;
	}
	mapper->enableBackTracking = true;
	mapper->backTrackLimit = args.backtracklimit;

	//cout << "json_file_name = " << json_file_name << "\n";
	// exit(EXIT_SUCCESS);

	auto start = chrono::steady_clock::now();

	bool mappingSuccess = false;
	bool binaryEmissionFailedAtII = false;
	auto try_map_at_current_ii = [&](bool strict_bookkeeping, bool emit_binary, const std::string &phase_label) -> bool
	{
		for (int restart = 0; restart < attempts_per_ii; ++restart)
		{
			DFG tempDFG;
			tempDFG.parseXML(inputDFG_filename);
			tempDFG.printDFG();

			CGRA *tempCGRA;
			if (json_file_name.empty())
			{
				tempCGRA = new CGRA(NULL, "coreCGRA", II, ydim, xdim, &tempDFG, PEType, numberOfDPs, mapper->getcongestedPortsPtr());
			}
			else
				{
					tempCGRA = new CGRA(json_file_name, II, xdim, ydim, mapper->getcongestedPortsPtr());
				}
				if (!apply_spm_mode(tempCGRA))
				{
					delete tempCGRA;
					return false;
				}

				tempCGRA->max_hops = args.max_hops;

			mapper->setRestartOffset(restart);
			mapper->setStrictBookkeeping(strict_bookkeeping);
			mapper->getcongestedPortsPtr()->clear();
			mapper->getconflictedPortsPtr()->clear();
			tempCGRA->analyzeTimeDist(tdi);

				bool phaseSuccess = false;
				std::string phaseFailureReason;
				try
				{
					if (mapping_method == 0)
					{
						phaseSuccess = mapper->Map(tempCGRA, &tempDFG);
					}
					else if (mapping_method == 1)
					{
						SAMapper * sa_mapper = static_cast<SAMapper*>(mapper);
						phaseSuccess = sa_mapper->SAMap(tempCGRA, &tempDFG);
					}
				}
				catch (const std::exception &e)
				{
					phaseSuccess = false;
					phaseFailureReason = e.what();
				}
				catch (...)
				{
					phaseSuccess = false;
					phaseFailureReason = "unknown non-standard exception";
				}

				mapper->congestionInfoFile.close();

				if (!phaseSuccess)
				{
					if (!phaseFailureReason.empty())
					{
						std::cerr << "Warning: mapper attempt failed at II=" << II
						          << " (" << phase_label << "): " << phaseFailureReason << "\n";
					}
					delete tempCGRA;
						if (restart + 1 < attempts_per_ii)
						{
							std::cout << "Restarting same II = " << II
							          << " (" << phase_label << ", attempt " << (restart + 2) << "/" << attempts_per_ii << ")\n";
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
					mapper->sanityCheck();
				}
				catch (const std::exception &e)
				{
					std::cerr << "Warning: sanity check failed at II=" << II
					          << " (" << phase_label << "): " << e.what() << "\n";
					delete tempCGRA;
					if (restart + 1 < attempts_per_ii)
					{
						std::cout << "Restarting same II = " << II
						          << " (" << phase_label << ", attempt " << (restart + 2) << "/" << attempts_per_ii << ")\n";
					}
					continue;
				}

			if (!emit_binary)
			{
				delete tempCGRA;
				return true;
			}

			if (PEType == "HyCUBE_4REG")
			{
				std::cout << "\nPrinting HyCUBE Binary...\n";
				try
				{
					mapper->printHyCUBEBinary(tempCGRA);
				}
					catch (const std::runtime_error &e)
					{
						std::cerr << "Warning: binary generation failed at II=" << II
						          << " (" << e.what() << "), retrying same II restart.\n";
						binaryEmissionFailedAtII = true;
						delete tempCGRA;
						if (restart + 1 < attempts_per_ii)
						{
							std::cout << "Restarting same II = " << II
							          << " (" << phase_label << ", attempt " << (restart + 2) << "/" << attempts_per_ii << ")\n";
						}
					continue;
				}
			}
			delete tempCGRA;
			return true;
		}
		return false;
	};

	while (!mappingSuccess)
	{
		binaryEmissionFailedAtII = false;
		if (use_relaxed_heuristic && mapping_method == 0)
		{
			mappingSuccess = try_map_at_current_ii(false, true, "relaxed");
			if (mappingSuccess)
			{
				break;
			}
			std::cout << "Relaxed mode failed at II = " << II
			          << "; retrying same II with strict bookkeeping fallback.\n";
		}

		// Strict fallback path if relaxed fails (or for non-PathFinder mappers).
		mappingSuccess = try_map_at_current_ii(true, true, "strict-fallback");
		if (mappingSuccess)
		{
			break;
		}

		for (DFGNode &node : currDFG.nodeList)
		{
			assert(node.rootDP == NULL);
		}

		II++;

		if (II == max_II)
		{
			std::cout << "### cannot map:  II max has been reached and exiting...\n";
			break;
		}

		if (!binaryEmissionFailedAtII && II > mapper->upperboundII)
		{
			std::cout << "upperbound II reached : " << mapper->upperboundII << "\n";
			std::cout << "Please use the mapping with II = " << mapper->upperboundFoundBy << ",with Iter = " << mapper->upperboundIter << "\n";
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
	std::ofstream result_file;
	result_file.open ("result.txt", std::ios_base::app); 
	result_file<< arch_name <<" "<<inputDFG_filename
		<<" method:"<<mapper->getMappingMethodName()<<" "<<II<<" "
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
