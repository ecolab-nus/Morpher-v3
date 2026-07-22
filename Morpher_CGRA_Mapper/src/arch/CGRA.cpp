/*
 * cgra.cpp
 *
 *  Created on: 20 Feb 2018
 *      Author: manupa
 */

#include <morpher/arch/CGRA.h>
#include <morpher/arch/LayoutObservation.h>
#include <morpher/arch/PE.h>
#include <morpher/arch/Port.h>
#include <morpher/arch/DataPath.h>
#include <morpher/dfg/DFG.h>
#include <morpher/util/tinyxml2.h>
#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <unordered_map>
#include <queue>
#include <functional>
#include <algorithm>
//#include <nlohmann/json.hpp>
#include <iomanip>
#include <regex>

using json = nlohmann::json;
using namespace CGRAXMLCompile;

namespace CGRAXMLCompile
{

//CGRA::CGRA() {
//	// TODO Auto-generated constructor stub
//
//}

} /* namespace CGRAXMLCompile */

namespace
{
	std::unordered_map<std::string, std::shared_ptr<const json>> g_preprocessed_arch_cache;

	struct TimeDistSearchResult
	{
		std::unordered_map<std::string, int> peTimeDist;
		int closestMemTimeDist = INT32_MAX;
	};

	struct PortDistEntry
	{
		int dist;
		Port *port;
	};

	struct PortDistGreater
	{
		bool operator()(const PortDistEntry &lhs, const PortDistEntry &rhs) const
		{
			return lhs.dist > rhs.dist;
		}
	};

	// Single-source Dijkstra over the MRRG port graph. Finds distances from
	// srcPE to all zerothSpatialPEs in one pass: O(E log V) instead of O(N * E).
	TimeDistSearchResult computeTimeDistFromSourcePE(PE *srcPE,
	                                                 const std::vector<PE *> &zerothSpatialPEs)
	{
		TimeDistSearchResult result;
		for (PE *destPE : zerothSpatialPEs)
			result.peTimeDist[destPE->getName()] = INT32_MAX;

		std::unordered_map<Port *, int> bestDistToPort;
		std::priority_queue<PortDistEntry, std::vector<PortDistEntry>, PortDistGreater> worklist;
		for (Port *op : srcPE->outputPorts)
		{
			bestDistToPort[op] = 0;
			worklist.push({0, op});
		}

		while (!worklist.empty())
		{
			PortDistEntry curr = worklist.top();
			worklist.pop();
			auto it = bestDistToPort.find(curr.port);
			if (it == bestDistToPort.end() || it->second != curr.dist)
				continue;

			Module *currMod = curr.port->getMod();
			assert(currMod);
			PE *currPE = currMod->getPE();
			FU *currFU = currMod->getFU();
			if (currFU != NULL && currPE != NULL)
			{
				auto peIt = result.peTimeDist.find(currPE->getName());
				if (peIt != result.peTimeDist.end() && curr.dist < peIt->second)
					peIt->second = curr.dist;
				if (currFU->isMEMFU() && curr.dist < result.closestMemTimeDist)
					result.closestMemTimeDist = curr.dist;
			}

			std::vector<Port *> nextPorts = currMod->getNextPorts(curr.port);
			for (Port *nextPort : nextPorts)
			{
				int timeDelta = 0;
				if (nextPort->getMod()->get_t() != currMod->get_t())
					timeDelta = 1;
				if (curr.port->getType() == REGO && nextPort->getType() == REGI)
					timeDelta = 1;

				const int nextDist = curr.dist + timeDelta;
				auto nextIt = bestDistToPort.find(nextPort);
				if (nextIt == bestDistToPort.end() || nextDist < nextIt->second)
				{
					bestDistToPort[nextPort] = nextDist;
					worklist.push({nextDist, nextPort});
				}
			}
		}

		return result;
	}
}

void CGRAXMLCompile::CGRA::createGenericCGRA(int x_max, int y_max, int t_max, std::string peType, int numberofDPs)
{

	this->x_max = x_max;
	this->y_max = y_max;
	this->t_max = t_max;

	this->minLatBetweenPEs = 1;
	if (peType.find("HyCUBE") != std::string::npos || peType.find("HYCUBE") != std::string::npos)
	{
		this->minLatBetweenPEs = 0;
	}

	for (int t = 0; t < t_max; ++t)
	{
		for (int y = 0; y < y_max; ++y)
		{
			for (int x = 0; x < x_max; ++x)
			{
				PE *newPE;
				std::string PE_name = "PE_" + std::to_string(t) + "," + std::to_string(y) + "," + std::to_string(x);

				// For distributed-memory architectures every PE is a memory PE.
				// The isMEMpe flag controls whether a PE gets FU_MEM capability.
				bool isMEMpe = true;
				newPE = new PE(this, PE_name, x, y, t, peType, isMEMpe, numberofDPs);
				subModules.push_back(newPE);
				PEArr[t][y][x] = newPE;
			}
		}
	}

	//create connections
	for (int t = 0; t < t_max; ++t)
	{
		for (int y = 0; y < y_max; ++y)
		{
			for (int x = 0; x < x_max; ++x)
			{

				PE *currPE = PEArr[t][y][x];
				if (x - 1 >= 0)
				{
					Port *currPE_WestO = currPE->getOutPort("WEST_O");
					PE *westPE = PEArr[t][y][x - 1];
					Port *westPE_EastI = westPE->getInPort("EAST_I");
						insertConnection(currPE_WestO, westPE_EastI);
				}

				if (x + 1 < x_max)
				{
					Port *currPE_EastO = currPE->getOutPort("EAST_O");
					PE *eastPE = PEArr[t][y][x + 1];
					Port *eastPE_WestI = eastPE->getInPort("WEST_I");
						insertConnection(currPE_EastO, eastPE_WestI);
				}

				if (y - 1 >= 0)
				{
					Port *currPE_NorthO = currPE->getOutPort("NORTH_O");
					PE *northPE = PEArr[t][y - 1][x];
					Port *northPE_SouthI = northPE->getInPort("SOUTH_I");
						insertConnection(currPE_NorthO, northPE_SouthI);
				}

				if (y + 1 < y_max)
				{
					Port *currPE_SouthO = currPE->getOutPort("SOUTH_O");
					PE *southPE = PEArr[t][y + 1][x];
					Port *southPE_NorthI = southPE->getInPort("NORTH_I");
						insertConnection(currPE_SouthO, southPE_NorthI);
				}

				int t_next = (t + 1) % t_max;
				PE *nextCyclePE = PEArr[t_next][y][x];
				for (RegFile *RF : currPE->allRegs)
				{
					for (int i = 0; i < RF->get_nRegs(); ++i)
					{
						Port *reg_i = nextCyclePE->getInPort(RF->getName() + "_REG_I" + std::to_string(i));
						Port *reg_o = currPE->getOutPort(RF->getName() + "_REG_O" + std::to_string(i));

							insertConnection(reg_o, reg_i);
						regCons[std::make_pair(reg_o, reg_i)] = true;
					}
				}

				for (std::pair<Port *, Port *> curr_portpair : currPE->getRegConPorts())
				{
					std::pair<Port *, Port *> next_portpair = nextCyclePE->getRegConPort(curr_portpair.first->getName());
					Module *mod = curr_portpair.first->getMod();
					mod->insertConnection(curr_portpair.second, next_portpair.first);
					mod->regCons[std::make_pair(curr_portpair.second, next_portpair.first)] = true;
				}

			}
		}
	}

}

std::set<CGRAXMLCompile::Port *> CGRAXMLCompile::CGRA::getConflictPorts(Port *p)
{
	if (conflictPorts.find(p) == conflictPorts.end())
	{
		std::set<Port *> emptyVec;
		conflictPorts[p] = emptyVec;
	}
	//	std::cout << "size = " << conflictPorts[p].size() << "\n";

	for (Port *tp : conflictPorts[p])
	{
		assert(tp != NULL);
		//		std::cout << tp->getName() << "\n";
		//		std::cout << tp->getFullName() << "\n";
	}

	return conflictPorts[p];
}

void CGRAXMLCompile::CGRA::insertConflictPort(Port *a, Port *b)
{
	assert(a != NULL);
	assert(b != NULL);
	std::cout << "insertConflict Port b : " << b->getFullName();
	std::cout << ", a : " << a->getFullName() << "\n";
	conflictPorts[a].insert(b);
}

void CGRAXMLCompile::CGRA::ParseCGRA(const json &cgra_desc, int II)
{

	//NOTE you can only have sub modules at the CGRA level.
	//You can always add functionality in terms of PEs.

	for (int t = 0; t < II; t++)
	{

		for (auto &el : cgra_desc["SUBMODS"].items())
		{

			//if not at CGRA level, a single instance suffice
			string type = el.key();
			for (auto &sm : el.value())
			{
				string name = sm["name"];
				name = name + "-T" + to_string(t);

				Module *submod;
				if (sm.find("X") != sm.end() && sm.find("Y") != sm.end())
				{
					int y = sm["Y"];
					int x = sm["X"];
					submod = ParseModule((*top_desc)["ARCH"][type], this, name, type, t, x, y);// y, x);
				}
				else
				{
					submod = ParseModule((*top_desc)["ARCH"][type], this, name, type, t);
				}
				// Module *submod = ParseModule(top_desc["ARCH"][type], this, name, type, t);
				assert(submod);

				// cout << "submod name = " << name << "\n";
				// cout << "submod real name = " << submod->getFullName() << "\n";

				Name2SubMod[name] = submod;
				subModArr[t].push_back(submod);
			}
		}

		auto internals_it = cgra_desc.find("INTERNALS");
		if (internals_it != cgra_desc.end())
		{
			for (auto &p : *internals_it)
			{
				Port *port_ptr = new Port(p, INT, this);
				this->Name2Port[p] = port_ptr;
				this->internalPorts.push_back(port_ptr);
			}
		}

		auto connections_it = cgra_desc.find("CONNECTIONS");
		if (connections_it != cgra_desc.end())
		{
			for (auto &el : connections_it->items())
			{
				string src = el.key();
				string src_mod_str = src.substr(0, src.find("."));
				string src_port_str = src.erase(0, src.find(".") + 1);

				Module *src_mod;
				if (src_mod_str == "THIS")
				{
					src_mod = this;
				}
				else
				{
					src_mod_str = src_mod_str + "-T" + to_string(t);
					src_mod = Name2SubMod[src_mod_str];
				}
				if (!src_mod)
				{
					LOG(JSONARCH) << "src mod =" << src_mod_str << ", cannot be found.\n";
				}
				assert(src_mod);

				for (string dest : el.value())
				{
					string dest_mod_str = dest.substr(0, dest.find("."));
					string dest_port_str = dest.erase(0, dest.find(".") + 1);

					Module *dest_mod;
					if (dest_mod_str == "THIS")
					{
						dest_mod = this;
					}
					else
					{
						dest_mod_str = dest_mod_str + "-T" + to_string(t);
						dest_mod = Name2SubMod[dest_mod_str];
					}
					LOG(JSONARCH)  << "dest mod str = " << dest_mod_str << "\n";
					assert(dest_mod);

					Port *src_p;
					if (!src_mod->Name2RegPort.empty() && src_mod->Name2RegPort.find(src_port_str) != src_mod->Name2RegPort.end())
					{
						src_p = src_mod->Name2RegPort[src_port_str].second;
					}
					else
					{
						src_p = src_mod->Name2Port[src_port_str];
					}
					assert(src_p);

					Port *dest_p;
					if (!dest_mod->Name2RegPort.empty() && dest_mod->Name2RegPort.find(dest_port_str) != dest_mod->Name2RegPort.end())
					{
						dest_p = dest_mod->Name2RegPort[dest_port_str].first;
					}
					else
					{
						dest_p = dest_mod->Name2Port[dest_port_str];
					}
					if (!dest_p)
						throw 20;
					this->insertConnection(src_p, dest_p);
				}
			}
		}
	}

	for (int t = 0; t < II; t++)
	{
		for (int i = 0; i < subModArr[t].size(); i++)
		{
			if (!dynamic_cast<PE *>(subModArr[t][i]))
				continue;

			PE *currPE = static_cast<PE *>(subModArr[t][i]);
			int next_t = (t + 1) % II;
			PE *nextCyclePE = static_cast<PE *>(subModArr[next_t][i]);

			NextCyclePEMap[currPE] = nextCyclePE;

			string curr_pe_name_prefix = currPE->getName().substr(0, currPE->getName().find("-"));
			string next_pe_name_prefix = nextCyclePE->getName().substr(0, nextCyclePE->getName().find("-"));
			assert(curr_pe_name_prefix == next_pe_name_prefix);

			for (std::pair<Port *, Port *> curr_portpair : currPE->getRegConPorts())
			{
				std::pair<Port *, Port *> next_portpair = nextCyclePE->getRegConPort(curr_portpair.first->getName());
				Module *mod = curr_portpair.first->getMod();
				mod->insertConnection(curr_portpair.second, next_portpair.first);
				mod->regCons[std::make_pair(curr_portpair.second, next_portpair.first)] = true;
			}
		}
	}
}

Module *CGRAXMLCompile::CGRA::ParseModule(const json &module_desc, Module *parent, string module_name, string type, int t, int x, int y)
{
	LOG(JSONARCH)  << "Parsing module  :: module name = " << module_name << ", type = " << type << "\n";
	assert(top_desc && !top_desc->empty());
	Module *ret;

	if (type == "DP" || type == "MDP")
	{
		ret = new DataPath(parent, module_name, t);
		ret->Name2Port["I1"] = ret->getInPort("I1");
		ret->Name2Port["I2"] = ret->getInPort("I2");
		ret->Name2Port["P"] = ret->getInPort("P");
		ret->Name2Port["T"] = ret->getOutPort("T");

		FU *ret_fu = ret->getFU();
		if (ret_fu->isMEMFU())
		{
			freeMemNodes++;
			freeMemNodeSet.insert((DataPath *)ret);
			PE *ret_pe = ret->getPE();
			ret_pe->isMemPE = true;
		}

		if (type == "MDP")
		{
			auto sockets_it = module_desc.find("SOCKETS");
			if (sockets_it != module_desc.end())
			{
				for (auto &p : *sockets_it)
				{
					Port *port_ptr = new Port(p, SOCKET, ret);
					ret->Name2Port[p] = port_ptr;
					ret->internalPorts.push_back(port_ptr);
				}
			}

			auto tsockets_it = module_desc.find("TSOCKETS");
			if (tsockets_it != module_desc.end())
			{
				for (auto &p : *tsockets_it)
				{
					Port *port_ptr = new Port(p, SOCKET, ret);
					ret->Name2Port[p] = port_ptr;
					ret->socketPorts.push_back(port_ptr);
				}
			}

			auto isockets_it = module_desc.find("ISOCKETS");
			if (isockets_it != module_desc.end())
			{
				for (auto &p : *isockets_it)
				{
					Port *port_ptr = new Port(p, SOCKET, ret);
					ret->Name2Port[p] = port_ptr;
					ret->socketPorts.push_back(port_ptr);
				}
			}
		}

		return ret;
	}
	else if (type.find("FU_") != string::npos || type == "FU")
	{
		ret = new FU(parent, module_name, t);
	}
	else if ((type.find("PE_") != string::npos) || type == "PE")
	{
		assert(type.find("PE") != string::npos);
		assert(x != -1);
		assert(y != -1);
		ret = new PE(parent, module_name, t, x, y);
	}
	else if (type == "CGRA")
	{
		ret = this;
	}
	else
	{
		ret = new Module(parent, module_name, type, t);
	}

	auto inputs_it = module_desc.find("INPUTS");
	if (inputs_it != module_desc.end())
	{
		for (auto &p : *inputs_it)
		{
			Port *port_ptr = new Port(p, IN, ret);
			ret->Name2Port[p] = port_ptr;
			ret->inputPorts.push_back(port_ptr);
		}
	}

	auto outputs_it = module_desc.find("OUTPUTS");
	if (outputs_it != module_desc.end())
	{
		for (auto &p : *outputs_it)
		{
			Port *port_ptr = new Port(p, OUT, ret);
			ret->Name2Port[p] = port_ptr;
			ret->outputPorts.push_back(port_ptr);
		}
	}

	auto internals_it = module_desc.find("INTERNALS");
	if (internals_it != module_desc.end())
	{
		for (auto &p : *internals_it)
		{
			Port *port_ptr = new Port(p, INT, ret);
			ret->Name2Port[p] = port_ptr;
			ret->internalPorts.push_back(port_ptr);
		}
	}

	auto sockets_it = module_desc.find("SOCKETS");
	if (sockets_it != module_desc.end())
	{
		for (auto &p : *sockets_it)
		{
			Port *port_ptr = new Port(p, SOCKET, ret);
			ret->Name2Port[p] = port_ptr;
			ret->socketPorts.push_back(port_ptr);
		}
	}

	auto tsockets_it = module_desc.find("TSOCKETS");
	if (tsockets_it != module_desc.end())
	{
		for (auto &p : *tsockets_it)
		{
			Port *port_ptr = new Port(p, SOCKET, ret);
			ret->Name2Port[p] = port_ptr;
			ret->socketPorts.push_back(port_ptr);
		}
	}

	auto isockets_it = module_desc.find("ISOCKETS");
	if (isockets_it != module_desc.end())
	{
		for (auto &p : *isockets_it)
		{
			Port *port_ptr = new Port(p, SOCKET, ret);
			ret->Name2Port[p] = port_ptr;
			ret->socketPorts.push_back(port_ptr);
		}
	}

	auto regs_it = module_desc.find("REGS");
	if (regs_it != module_desc.end())
	{
		for (auto &p : *regs_it)
		{
			assert(ret->getPE());
			ret->insertRegPort(p);
			ret->Name2RegPort[p] = ret->getRegPort(p);
		}
	}

	auto ops_it = module_desc.find("OPS");
	if (ops_it != module_desc.end())
	{
		for (auto &el : ops_it->items())
		{
			FU *ret_fu = static_cast<FU *>(ret);
			string OP = el.key();
			int lat = el.value();
			ret_fu->supportedOPs[OP] = lat;
			insertGlobalOP(OP, lat);
		}
	}

	auto submods_it = module_desc.find("SUBMODS");
	if (submods_it != module_desc.end())
	{
		for (auto &el : submods_it->items())
		{
			string child_type = el.key();
			for (auto &sm : el.value())
			{
				string name = sm["name"];
				Module *submod;
				if (sm.find("X") != sm.end() && sm.find("Y") != sm.end())
				{
					int child_y = sm["Y"];
					int child_x = sm["X"];
					submod = ParseModule((*top_desc)["ARCH"][child_type], ret, name, child_type, t, child_x, child_y);
				}
				else
				{
					submod = ParseModule((*top_desc)["ARCH"][child_type], ret, name, child_type, t);
					if (child_type == "SPM")
					{
						submod->isSPM = true;
						auto data_layout_it = module_desc.find("DATA_LAYOUT");
						if (data_layout_it != module_desc.end())
						{
							for (auto &layout_el : data_layout_it->items())
							{
								LOG(JSONARCH) << "Inserting to module = " << submod->getName() << ", data_layout var name = " << static_cast<std::string>(layout_el.key());
								LOG(JSONARCH) << ",size = " << (int)layout_el.value() << "\n";
								submod->data_layout.insert(static_cast<std::string>(layout_el.key()));
								this->Variable2SPM[static_cast<std::string>(layout_el.key())] = submod;
								this->Variable2SPMAddr[static_cast<std::string>(layout_el.key())] = (int)layout_el.value();
							}
						}
					}
				}

				ret->Name2SubMod[name] = submod;
				ret->subModules.push_back(submod);
			}
		}
	}

	auto conns_it = module_desc.find("CONNECTIONS");
	if (conns_it != module_desc.end())
	{
		for (auto &el : conns_it->items())
		{
			string src = el.key();
			Port *src_p = ret->getJSONPort(src, true);
			for (string dest : el.value())
			{
				Port *dest_p = ret->getJSONPort(dest, false);
				ret->insertConnection(src_p, dest_p);
			}
		}
	}

	auto conflicts_it = module_desc.find("CONFLICTS");
	if (conflicts_it != module_desc.end())
	{
		for (auto &el : conflicts_it->items())
		{
			string src = el.key();
			Port *src_p = ret->getJSONPort(src, true);
			for (string dest : el.value())
			{
				Port *dest_p = ret->getJSONPort(dest, false);
				this->insertConflictPort(src_p, dest_p);
				this->insertConflictPort(dest_p, src_p);
			}
		}
	}

	if (FU *ret_fu = dynamic_cast<FU *>(ret))
	{
		assert(!ret_fu->supportedOPs.empty());
	}

	return ret;
}

bool CGRAXMLCompile::CGRA::ParseJSONArch(string fileName, int II)
{
	this->t_max = II;

	std::stringstream ss(fileName);
	std::string token;
	vector<string> cont;
	while (std::getline(ss, token, '/'))
	{
		cont.push_back(token);
	}
	string cgra_json_name = *cont.rbegin();
	LOG(JSONARCH)  << "cgra_json_name = " << cgra_json_name << "\n";

	string filename_extention = cgra_json_name.substr(cgra_json_name.find("."));
	LOG(JSONARCH)  << "filename_extention = " << filename_extention << "\n";
	assert(filename_extention == ".json");
	string filename_withoutext = cgra_json_name.substr(0, cgra_json_name.size() - 5);

	const char *dump_verbose_env = std::getenv("MORPHER_DUMP_VERBOSE_JSON");
	bool dump_verbose_json = false;
	if (dump_verbose_env != nullptr && dump_verbose_env[0] != '\0')
	{
		dump_verbose_json = (std::string(dump_verbose_env) != "0");
	}

	auto cache_it = g_preprocessed_arch_cache.find(fileName);
	if (cache_it != g_preprocessed_arch_cache.end())
	{
		top_desc = cache_it->second;
	}
	else
	{
		ifstream json_file(fileName.c_str());
		assert(json_file.is_open());
		json parsed_json;
		json_file >> parsed_json;

		PreprocessPattern(parsed_json["ARCH"]["CGRA"]);
		PreprocessInterSubmodConns(parsed_json["ARCH"]);

		if (dump_verbose_json)
		{
			string verbose_json_filename = filename_withoutext + "_verbose.json";
			ofstream verbose_json_file(verbose_json_filename.c_str());
			verbose_json_file << setw(4) << parsed_json << std::endl;
			verbose_json_file.close();
		}

		top_desc = std::make_shared<json>(std::move(parsed_json));
		g_preprocessed_arch_cache[fileName] = top_desc;
	}

	LOG(JSONARCH)  << "TOP_ARCH :: begin %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";
	// std::cout<< setw(4) << top_desc << std::endl;
	LOG(JSONARCH)  << "TOP_ARCH :: end %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";

	//just creating CGRA instance for t
	// ParseModule(json["ARCH"]["CGRA"], NULL, "CGRA_Ins", "CGRA", 0);

	//TODO : remove hardcoding of II=2
	ParseCGRA((*top_desc)["ARCH"]["CGRA"], II);
	LOG(JSONARCH)  << "Parsing JSON Success!!!\n";


	// exit(EXIT_SUCCESS);
	// assert(false);

	buildMemPE2Bank();
	return true;
}

void CGRAXMLCompile::CGRA::buildMemPE2Bank()
{
	// Walk each memPE's MEMPORT socket to the SPM bank it's wired to.
	// Connection map (connectedTo) lives on the CGRA (this), populated by
	// insertConnection during CONNECTIONS parsing.
	//
	// In hycube-style arches the destination port's module is the SPM
	// wrapper (e.g. SPM_B0_WRAPPER_INS). The wrapper's isSPM=false; its
	// inner child of type SPM carries isSPM=true and is what Variable2SPM
	// points to. So we descend the wrapper to find the inner SPM. If the
	// wired module already has isSPM=true (single-level encoding), we use
	// it directly.
	std::function<Module*(Module*)> findInnerSpm = [&](Module *m) -> Module* {
		if (!m) return nullptr;
		if (m->isSPM) return m;
		for (Module *sm : m->subModules) {
			if (Module *r = findInnerSpm(sm))
				return r;
		}
		return nullptr;
	};
	memPE2Bank.clear();
	const auto &cmap = this->getconnectedTo();
	for (PE *pe : getAllPEListDeterministic())
	{
		if (!pe->isMemPE)
			continue;
		Port *memport = pe->getSocketPort("MEMPORT");
		if (!memport)
			continue;
		auto it = cmap.find(memport);
		if (it == cmap.end() || it->second.empty())
			continue;
		Module *wired = it->second.front()->getMod();
		Module *bank = findInnerSpm(wired);
		// Fallback: walk up if neither the wired module nor its descendants
		// declare isSPM (defensive for unusual encodings).
		if (!bank) {
			bank = wired;
			while (bank && !bank->isSPM && bank->getParent())
				bank = (Module *)bank->getParent();
			if (bank && !bank->isSPM)
				bank = nullptr;
		}
		if (bank)
			memPE2Bank[pe] = bank;
	}
}

// Canonical bank identity. CGRA modules are time-replicated by ParseCGRA
// (II copies, each with its own wrapper instance named "<base>-T<n>"), so
// the inner-SPM Module* pointer differs across time steps even though all
// instances represent the same physical bank. Canonicalize by walking to
// the wrapper parent and stripping the "-T<digits>" suffix.
std::string CGRAXMLCompile::CGRA::canonicalBankKey(Module *bank)
{
	if (!bank) return std::string();
	Module *wrapper = (Module *)bank->getParent();
	std::string n = wrapper ? wrapper->getName() : bank->getName();
	size_t pos = n.rfind("-T");
	if (pos != std::string::npos && pos + 2 < n.size()) {
		bool all_digits = true;
		for (size_t i = pos + 2; i < n.size(); ++i) {
			if (!isdigit((unsigned char)n[i])) { all_digits = false; break; }
		}
		if (all_digits) n.resize(pos);
	}
	return n;
}

void ExpandPattern(json &input, json &connections, json &submods)
{

	assert(input["PATTERN"] == "GRID");

	int xdim = input["DIMS"]["X"];
	int ydim = input["DIMS"]["Y"];

	unordered_map<int, unordered_map<int, pair<string, string>>> submod_grid;

	for (auto &el : input["MODS"])
	{
		int x = el["X"];
		int y = el["Y"];
		string mod_type = el["MOD"];
		string mod_name;

		if (el.find("name") != el.end())
		{
			mod_name = el["name"];
		}
		else
		{
			mod_name = mod_type + "_X" + to_string(x) + "|" + "_Y" + to_string(y) + "|";
		}

		json mod;
		mod["name"] = mod_name;
		mod["X"] = x;
		mod["Y"] = y;

		submods[mod_type].push_back(mod);
		submod_grid[x][y] = make_pair(mod_type, mod_name);
	}

	for (auto &el : input["CONNECTIONS"])
	{
		string fromx_str = el["FROM_X"];
		int fromx = atoi(fromx_str.substr(1, fromx_str.size() - 1).c_str());
		if (fromx_str.size() == 1)
			fromx = 0;

		string fromy_str = el["FROM_Y"];
		int fromy = atoi(fromy_str.substr(1, fromy_str.size() - 1).c_str());
		if (fromy_str.size() == 1)
			fromy = 0;

		string curr_port_str = el["FROM_PORT"];

		string tox_str = el["TO_X"];
		int tox = atoi(tox_str.substr(1, tox_str.size() - 1).c_str());
		if (tox_str.size() == 1)
			tox = 0;

		string toy_str = el["TO_Y"];
		int toy = atoi(toy_str.substr(1, toy_str.size() - 1).c_str());
		if (toy_str.size() == 1)
			toy = 0;

		string to_port_str = el["TO_PORT"];

		LOG(JSONARCH)  << "fromx = " << fromx << ",";
		LOG(JSONARCH)  << "fromy = " << fromy << ",";
		LOG(JSONARCH)  << "tox = " << tox << ",";
		LOG(JSONARCH)  << "toy = " << toy << "\n";

		for (int y = 0; y < ydim; y++)
		{
			for (int x = 0; x < xdim; x++)
			{
				int src_x = x + fromx;
				int src_y = y + fromy;
				int dest_x = x + tox;
				int dest_y = y + toy;

				if ((src_x >= 0) && (src_x < xdim) &&
					(src_y >= 0) && (src_y < ydim) &&
					(dest_x >= 0) && (dest_x < xdim) &&
					(dest_y >= 0) && (dest_y < ydim))
				{
					string conn_from_str = submod_grid[src_x][src_y].second + "." + curr_port_str;
					string conn_next_str = submod_grid[dest_x][dest_y].second + "." + to_port_str;
					connections[conn_from_str].push_back(conn_next_str);
				}
			}
		}
	}
}

bool CGRAXMLCompile::CGRA::PreprocessPattern(json &top)
{

	json submods;
	json connections;

	LOG(JSONARCH)  << "Before JSON Begin :: %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";
	// std::cout << setw(4) << top << std::endl;
	LOG(JSONARCH)  << "Before JSON End :: %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";

	for (auto &el : top["SUBMODS"])
	{
		if (el.find("PATTERN") != el.end())
		{
			ExpandPattern(el, connections, submods);
		}
		else
		{
			for (auto &el2 : el.items())
			{
				auto key = el2.key();
				auto value = el2.value();
				submods[key] = value;
			}
		}
	}

	top.erase("SUBMODS");
	top["SUBMODS"] = submods;

	// top["CONNECTIONS"] = connections;
	for (auto &el : connections.items())
	{
		auto key = el.key();
		auto value = el.value();
		top["CONNECTIONS"][key] = value;
	}

	LOG(JSONARCH)  << "After JSON Begin :: %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";
	// std::cout << setw(4) << top << std::endl;
	LOG(JSONARCH)  << "After JSON End :: %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";
}

std::vector<PE *> CGRAXMLCompile::CGRA::getAllPEListDeterministic()
{
	std::vector<PE *> ret;

	if (!subModArr.empty())
	{
		for (auto it = subModArr.begin(); it != subModArr.end(); it++)
		{
			for (Module *m : it->second)
			{
				if (PE *pe = dynamic_cast<PE *>(m))
				{
					ret.push_back(pe);
				}
			}
		}
	}
	else
	{
		assert(!PEArr.empty());
		for (int t = 0; t < this->get_t_max(); ++t)
		{
			for (int y = 0; y < this->y_max; ++y)
			{
				for (int x = 0; x < this->x_max; ++x)
				{
					ret.push_back(PEArr[t][y][x]);
				}
			}
		}
	}

	std::sort(ret.begin(), ret.end(), [](PE *lhs, PE *rhs) {
		if (lhs == rhs)
			return false;
		if (lhs == nullptr)
			return true;
		if (rhs == nullptr)
			return false;
		if (lhs->T != rhs->T)
			return lhs->T < rhs->T;
		if (lhs->Y != rhs->Y)
			return lhs->Y < rhs->Y;
		if (lhs->X != rhs->X)
			return lhs->X < rhs->X;
		return lhs->getFullName() < rhs->getFullName();
	});
	ret.erase(std::unique(ret.begin(), ret.end()), ret.end());
	return ret;
}

unordered_set<PE *> CGRAXMLCompile::CGRA::getAllPEList()
{
	std::vector<PE *> ordered = getAllPEListDeterministic();
	return unordered_set<PE *>(ordered.begin(), ordered.end());
}

PE *CGRAXMLCompile::CGRA::getPE(int t, int y, int x)
{

	assert(!PEArr.empty());
	return PEArr[t][y][x];
}

PE *CGRAXMLCompile::CGRA::getLatencyPE(PE *currPE, int lat)
{
	if (!subModArr.empty())
	{
		PE *ret = currPE;
		while (lat > 0)
		{
			ret = NextCyclePEMap[ret];
			lat--;
		}
		return ret;
	}
	else
	{
		assert(!PEArr.empty());
		int curr_x = currPE->X;
		int curr_y = currPE->Y;
		int curr_t = currPE->T;

		int next_t = (curr_t + 1) % get_t_max();
		return PEArr[next_t][curr_y][curr_x];
	}
}

vector<PE *> CGRAXMLCompile::CGRA::getSpatialPEList(int t)
{
	vector<PE *> ret;

	if (!subModArr.empty())
	{
		auto it = subModArr[t];
		for (Module *m : it)
		{
			if (PE *pe = dynamic_cast<PE *>(m))
			{
				ret.push_back(pe);
			}
		}
	}
	else
	{
		assert(!PEArr.empty());
		for (int y = 0; y < this->y_max; ++y)
		{
			for (int x = 0; x < this->x_max; ++x)
			{
				ret.push_back(PEArr[t][y][x]);
			}
		}
	}

	return ret;
}

void CGRAXMLCompile::CGRA::insertGlobalOP(string OP, int lat)
{
	if (GlobalOPMinLatencyMap.find(OP) != GlobalOPMinLatencyMap.end())
	{
		if (lat != GlobalOPMinLatencyMap[OP])
		{
			LOG(JSONARCH)  << "op = " << OP << ", old lat = " << GlobalOPMinLatencyMap[OP] << ", new lat = " << lat << "\n";
			if (lat < GlobalOPMinLatencyMap[OP])
			{
				GlobalOPMinLatencyMap[OP] = lat;
			}
		}
		// assert(lat == GlobalOPMinLatencyMap[OP]);
	}
	else
	{
		GlobalOPMinLatencyMap[OP] = lat;
	}
}

string CGRAXMLCompile::CGRA::getCGRAName()
{

	if (subModArr.empty())
	{
		//old arch desc
		return peType + "_DP" + std::to_string(numberofDPs) + "_XDim=" + std::to_string(x_max) + "_YDim=" + std::to_string(y_max) + "_II=" + std::to_string(t_max);
	}
	else
	{
		std::stringstream ss(json_file);
		std::string token;
		vector<string> cont;
		while (std::getline(ss, token, '/'))
		{
			cont.push_back(token);
		}
		return *cont.rbegin();
	}
}

void CGRAXMLCompile::CGRA::analyzeTimeDist(TimeDistInfo tdi)
{
	TimeDistBetweenPEMap = tdi.TimeDistBetweenPEMap;
	TimeDistBetweenClosestMEMPEMap = tdi.TimeDistBetweenClosestMEMPEMap;
	
	if (get_t_max() == 1)
	{
		for (Module *m1 : subModArr[0])
		{
			PE *zeroth_pe = static_cast<PE *>(m1);
			MapTzeroPE[zeroth_pe] = zeroth_pe;
		}
	}

	if (get_t_max() > 1)
	{
		for (Module *m1 : subModArr[0])
		{
			PE *zeroth_pe = static_cast<PE *>(m1);
			MapTzeroPE[zeroth_pe] = zeroth_pe;
			PE *currPE = NextCyclePEMap[zeroth_pe];
			for (int i = 1; i < get_t_max(); i++)
			{
				MapTzeroPE[currPE] = zeroth_pe;
				currPE = NextCyclePEMap[currPE];
			}
		}
	}

	LOG(ROUTE) << "Timing analysis between PEs done.!\n";
	for (auto it1 = TimeDistBetweenPEMap.begin(); it1 != TimeDistBetweenPEMap.end(); it1++)
	{
		string pe1 = it1->first;
		for (auto it2 = TimeDistBetweenPEMap[pe1].begin(); it2 != TimeDistBetweenPEMap[pe1].end(); it2++)
		{
			string pe2 = it2->first;
			int time_dist = it2->second;
			LOG(ROUTE) << "PE1=" << pe1 << "\tPE2=" << pe2 << "\ttime_dist=" << time_dist ;
		}
	}

	LOG(ROUTE) << "Timing analysis between PE and closest memPE :: \n";
	for (auto it = TimeDistBetweenClosestMEMPEMap.begin(); it != TimeDistBetweenClosestMEMPEMap.end(); it++)
	{
		string pe = it->first;
		int time_dist = it->second;
		LOG(ROUTE) << "PE=" << pe << "\ttime_dist=" << time_dist ;
	}

	IntraPETimeDistAnalysisDone = true;
}

TimeDistInfo CGRAXMLCompile::CGRA::analyzeTimeDist()
{
	std::stringstream output_ss;
	output_ss<<"initial time analayze start\n";

	std::vector<PE *> zerothSpatialPEs;
	for (Module *m1 : subModArr[0])
	{
		if (PE *pe = dynamic_cast<PE *>(m1))
			zerothSpatialPEs.push_back(pe);
	}

	for (PE *srcPE : zerothSpatialPEs)
	{
		TimeDistSearchResult srcResult = computeTimeDistFromSourcePE(srcPE, zerothSpatialPEs);
		TimeDistBetweenClosestMEMPEMap[srcPE->getName()] = srcResult.closestMemTimeDist;
		for (PE *destPE : zerothSpatialPEs)
		{
			int dist = srcResult.peTimeDist[destPE->getName()];
			if (dist == INT32_MAX)
			{
				// Fallback to legacy per-pair traversal only when the graph search misses a route.
				dist = getTimeDistBetweenPEs(srcPE, destPE);
			}
			TimeDistBetweenPEMap[srcPE->getName()][destPE->getName()] = dist;
			output_ss << srcPE->getName() << " -> " << destPE->getName() << " dist:" << dist << "\n";
		}
	}
	LOG(ROUTE)<<output_ss.str();
	if (get_t_max() == 1)
	{
		for (Module *m1 : subModArr[0])
		{
			PE *zeroth_pe = static_cast<PE *>(m1);
			MapTzeroPE[zeroth_pe] = zeroth_pe;
		}
	}

	if (get_t_max() > 1)
	{
		for (Module *m1 : subModArr[0])
		{
			PE *zeroth_pe = static_cast<PE *>(m1);
			MapTzeroPE[zeroth_pe] = zeroth_pe;
			PE *currPE = NextCyclePEMap[zeroth_pe];
			for (int i = 1; i < get_t_max(); i++)
			{
				MapTzeroPE[currPE] = zeroth_pe;
				currPE = NextCyclePEMap[currPE];
			}
		}
	}
	output_ss<<"initial time analayze done\n";
	LOG(ROUTE)<<output_ss.str();

	LOG(ROUTE) << "Timing analysis between PEs done.!\n";
	for (auto it1 = TimeDistBetweenPEMap.begin(); it1 != TimeDistBetweenPEMap.end(); it1++)
	{
		string pe1 = it1->first;
		for (auto it2 = TimeDistBetweenPEMap[pe1].begin(); it2 != TimeDistBetweenPEMap[pe1].end(); it2++)
		{
			string pe2 = it2->first;
			int time_dist = it2->second;
			LOG(ROUTE)<< "PE1=" << pe1 << "\tPE2=" << pe2 << "\ttime_dist=" << time_dist << "";
		}
	}

	LOG(ROUTE) << "Timing analysis between PE and closest memPE :: \n";
	for (auto it = TimeDistBetweenClosestMEMPEMap.begin(); it != TimeDistBetweenClosestMEMPEMap.end(); it++)
	{
		string pe = it->first;
		int time_dist = it->second;
		LOG(ROUTE) << "PE=" << pe << "\ttime_dist=" << time_dist << "";
	}


	

	IntraPETimeDistAnalysisDone = true;
	TimeDistInfo tdi;
	tdi.TimeDistBetweenClosestMEMPEMap = TimeDistBetweenClosestMEMPEMap;
	tdi.TimeDistBetweenPEMap = TimeDistBetweenPEMap;
	return tdi;
	// exit(EXIT_SUCCESS);
}

void CGRAXMLCompile::CGRA::traverseUntil(PE *srcPE, PE *destPE, Port *currPort, int time_dist, unordered_map<Port *, int> &already_traversed, int &result)
{

	auto in_rectangle_range =  [ &]( Port *port){
		int port_x = port->getPE()->getPosition_X();
		int port_y = port->getPE()->getPosition_Y();
		bool in_range = true;

		//release the constraint by +1/-1 as some memory PE might cause a problem
		int max_x = std::max(srcPE->getPosition_X(), destPE->getPosition_X()) + 1;
		int min_x = std::min(srcPE->getPosition_X(), destPE->getPosition_X()) -1 ;
		int max_y = std::max(srcPE->getPosition_Y(), destPE->getPosition_Y()) + 1;
		int min_y = std::min(srcPE->getPosition_Y(), destPE->getPosition_Y()) -1 ;

		if(port_x > max_x || port_x < min_x){
			in_range = false;
		}

		if(port_y > max_y || port_y < min_y){
			in_range = false;
		}


		return in_range;
	};

	if (already_traversed.find(currPort) != already_traversed.end())
	{
		if (time_dist < already_traversed[currPort])
		{
			// cout << "srcPE=" << srcPE->getName() << ",destPE=" << destPE->getName() << "\tcurrPort=" << currPort->getFullName() << ",time_dist=" << time_dist << "\n";
			already_traversed[currPort] = time_dist;
		}
		else
		{
			return;
		}
	}
	else
	{
		// cout << "srcPE=" << srcPE->getName() << ",destPE=" << destPE->getName() << "\tcurrPort=" << currPort->getFullName() << ",time_dist=" << time_dist << "\n";
		already_traversed[currPort] = time_dist;
	}

	PE *currPE = currPort->getMod()->getPE();
	FU *currFU = currPort->getMod()->getFU();

	if (currFU != NULL && currFU->isMEMFU())
	{
		if (time_dist < TimeDistBetweenClosestMEMPEMap[srcPE->getName()])
		{
			// cout << "fu = " << currFU->getFullName() << ",";
			// cout << "srcPE=" << srcPE->getName() << ",destPE=" << destPE->getName() << "\tcurrPort=" << currPort->getFullName() << ",time_dist=" << time_dist << "\n";
			TimeDistBetweenClosestMEMPEMap[srcPE->getName()] = time_dist;
		}
	}

	if (currFU != NULL && currPE == destPE)
	{
		// cout << "found destPE, srcPE=" << srcPE->getName() << ",destPE=" << destPE->getName() << "\tcurrPort=" << currPort->getFullName() << ",time_dist=" << time_dist << "\n";
		result = time_dist;
		return;
	}

	vector<Port *> nextPorts = currPort->getMod()->getNextPorts(currPort);
	for (Port *p : nextPorts)
	{
		if(!in_rectangle_range(p)){
			continue;
		}
		int time_delta = 0;
		if (p->getMod()->get_t() != currPort->getMod()->get_t())
		{
			time_delta = 1;
		}
		if (currPort->getType() == REGO && p->getType() == REGI)
		{
			time_delta = 1;
		}
		assert(p != currPort);
		traverseUntil(srcPE, destPE, p, time_dist + time_delta, already_traversed, result);
	}
}

int CGRAXMLCompile::CGRA::getTimeDistBetweenPEs(PE *srcPE, PE *destPE)
{
	int ret_val = INT32_MAX;
	for (Port *op : srcPE->outputPorts)
	{
		unordered_map<Port *, int> already_traversed;
		int tmp = 0;
		traverseUntil(srcPE, destPE, op, 0, already_traversed, tmp);
		if (tmp != -1 && tmp < ret_val)
			ret_val = tmp;
	}
	assert(ret_val != INT32_MAX);
	return ret_val;
}

int CGRAXMLCompile::CGRA::getTimeClosestMEMPE(PE *currPE)
{
	assert(IntraPETimeDistAnalysisDone);
	PE *zeroth_currPE = MapTzeroPE[currPE];
	assert(TimeDistBetweenClosestMEMPEMap.find(zeroth_currPE->getName()) != TimeDistBetweenClosestMEMPEMap.end());

	return TimeDistBetweenClosestMEMPEMap[zeroth_currPE->getName()];
}

int CGRAXMLCompile::CGRA::getQuickTimeDistBetweenPEs(PE *srcPE, PE *destPE)
{
	assert(IntraPETimeDistAnalysisDone);

	PE *zeroth_srcPE = MapTzeroPE[srcPE];
	PE *zeroth_destPE = MapTzeroPE[destPE];

	assert(zeroth_srcPE);
	assert(zeroth_destPE);

	// cout << "zeroth_srcPE = " << zeroth_srcPE->getFullName() << "\n";
	// cout << "zeroth_destPE = " << zeroth_destPE->getFullName() << "\n";

	assert(TimeDistBetweenPEMap.find(zeroth_srcPE->getName()) != TimeDistBetweenPEMap.end());
	assert(TimeDistBetweenPEMap[zeroth_srcPE->getName()].find(zeroth_destPE->getName()) != TimeDistBetweenPEMap[zeroth_srcPE->getName()].end());

	//Hack to make the zeroth_srcPE == zeroth_destPE more closer
	if (zeroth_srcPE != zeroth_destPE)
	{
		return TimeDistBetweenPEMap[zeroth_srcPE->getName()][zeroth_destPE->getName()];
	}
	else
	{
		return TimeDistBetweenPEMap[zeroth_srcPE->getName()][zeroth_destPE->getName()] + 1;
	}
}

void CGRAXMLCompile::CGRA::EstablishTemporalLinkageModule(Module *curr_cycle_module, Module *next_cycle_module)
{
	curr_cycle_module->attachNextTimeIns(next_cycle_module);
	// cout << "Attaching :: curr_cycle_module = " << curr_cycle_module->getFullName() << "\t\t next_cycle_module = " << next_cycle_module->getFullName() << "\n";
	for (Module *curr_cycle_sub_module : curr_cycle_module->subModules)
	{
		Module *next_cycle_sub_module = next_cycle_module->getSubMod(curr_cycle_sub_module->getName());
		assert(next_cycle_sub_module);
		EstablishTemporalLinkageModule(curr_cycle_sub_module, next_cycle_sub_module);
	}
}

void CGRAXMLCompile::CGRA::EstablishTemporalLinkage()
{
	LOG(CGRA) << "EstablishTemporalLinkage started!.\n";
	if (!subModArr.empty())
	{
		//through parsed json
		for (int t = 0; t < t_max; t++)
		{
			for (Module *curr_cycle_pe_mod : subModArr[t])
			{
				if (!dynamic_cast<PE *>(curr_cycle_pe_mod))
					continue;
				PE *curr_cycle_pe = static_cast<PE *>(curr_cycle_pe_mod);
				PE *next_cycle_pe = NextCyclePEMap[curr_cycle_pe];
				EstablishTemporalLinkageModule(curr_cycle_pe, next_cycle_pe);
			}
		}
	}
	else
	{
		//classic cpp defintion of architectures
		assert(!PEArr.empty());
		for (int t = 0; t < t_max; t++)
		{
			for (int y = 0; y < y_max; y++)
			{
				for (int x = 0; x < x_max; x++)
				{
					PE *curr_cycle_pe = PEArr[t][y][x];
					PE *next_cycle_pe = PEArr[(t + 1) % t_max][y][x];
					EstablishTemporalLinkageModule(curr_cycle_pe, next_cycle_pe);
				}
			}
		}
	}

	LOG(CGRA) << "EstablishTemporalLinkage done!.\n";
}

void CGRAXMLCompile::CGRA::PrintMappedJSON(string fileName)
{

	json output_json;
	ofstream outFile(fileName);
	assert(outFile.is_open());

	vector<PE *> zeroth_spatial_pe_list = getSpatialPEList(0);

	output_json["II"] = get_t_max();
	InsertVariable2SPMAddrInfo(output_json);

	for (PE *pe : zeroth_spatial_pe_list)
	{
		output_json["CGRA_INS"]["TYPE"] = "CGRA";
		string pe_name = pe->getName();
		string spatial_pe_name = pe_name.substr(0, pe_name.size() - 3); //remove the last "-T0" component;
		LOG(JSONARCH)  <<"DMD PE NAME:"<< spatial_pe_name<<"\n";
		
		PrintMappedJSONModule(pe, output_json["CGRA_INS"]["SUBMODS"][spatial_pe_name]);
	}

	outFile << setw(4) << output_json << std::endl;
	outFile.close();
}

// by Yujie
void CGRAXMLCompile::CGRA::PrintMappingForPillars(string fileName_i, string fileName_r){
	DataPrepare();

	json output_json;
	ofstream outFile_i(fileName_i);
	ofstream outFile_r(fileName_r);
	ofstream outJsonFile(fileName_i+"mapping.json");
	assert(outJsonFile.is_open());
	assert(outFile_i.is_open());
	assert(outFile_r.is_open());

	vector<PE *> zeroth_spatial_pe_list = getSpatialPEList(0);
	output_json["II"] = get_t_max();
	InsertVariable2SPMAddrInfo(output_json);   // It seems that there is no influence from this code. 

	for (PE *pe : zeroth_spatial_pe_list){
		output_json["CGRA_INS"]["TYPE"] = "CGRA";
		string pe_name = pe->getName();
		string spatial_pe_name = pe_name.substr(0, pe_name.size() - 3); //remove the last "-T0" component;
		LOG(JSONARCH)  <<"DMD PE NAME:"<< spatial_pe_name<<"\n";

		PrintMappedPillarsModule(pe, output_json["CGRA_INS"]["SUBMODS"][spatial_pe_name], outFile_i);
	}
	outJsonFile << setw(4) << output_json << std::endl;
	outJsonFile.close();
	outFile_i.close();
	for (std::map<int, int*>::iterator iter = ConstRecord.begin(); iter != ConstRecord.end(); ++iter) {
		outFile_r<<"const"<<iter->first<<" "<<iter->second[0]<<":cgra.tile_0.pe_"<<iter->second[1]<<"_"<<iter->second[2]<<".const0.internalNode_0"<<" "<<iter->second[0]<<" 0"<<std::endl;
		delete []iter->second;
	}
	outFile_r.close();
}

void CGRAXMLCompile::CGRA::EmitLayoutObservation(DFG *dfg, string fileName)
{
	json out;
	out["kernel"] = getCGRAName();
	out["load_store_nodes"] = json::array();
	std::map<std::string, json> per_array;

	auto isMemOpInclusive = [](const std::string &op) {
		static const std::vector<std::string> prefixes = {
			"LOAD", "STORE", "OLOAD", "OSTORE",
			"VLOAD", "VSTORE", "VOLOAD", "VOSTORE"
		};
		for (auto &p : prefixes)
			if (op.rfind(p, 0) == 0) return true;
		return false;
	};
	for (DFGNode &node : dfg->nodeList)
	{
		if (!isMemOpInclusive(node.op))
			continue;

		json entry;
		entry["idx"] = node.idx;
		entry["op"] = node.op;
		entry["base_pointer"] = node.base_pointer_name;
		auto observed_base_it = Variable2SPMAddr.find(node.base_pointer_name);
		const int *observed_base =
			(observed_base_it != Variable2SPMAddr.end())
				? &observed_base_it->second
				: nullptr;
		const int observed_gep_offset = layoutObservationGepOffset(
			node.op, node.gep_offset, node.constant, observed_base);
		entry["gep_offset"] = observed_gep_offset;

		json mapped_pe = nullptr;
		if (node.rootDP && node.rootDP->getPE())
		{
			PE *pe = node.rootDP->getPE();
			mapped_pe = {
				{"x", pe->X},
				{"y", pe->Y},
				{"t", pe->T},
				{"is_memPE", pe->isMemPE}};
		}
		entry["mapped_pe"] = mapped_pe;

		const std::string &base = node.base_pointer_name;
		auto bank_it = Variable2SPM.find(base);
		entry["current_bank_module"] = (bank_it != Variable2SPM.end() && bank_it->second)
										   ? bank_it->second->getName()
										   : std::string();
		auto addr_it = Variable2SPMAddr.find(base);
		entry["current_spm_base_addr"] = (addr_it != Variable2SPMAddr.end())
											 ? addr_it->second
											 : -1;

		out["load_store_nodes"].push_back(entry);

		if (base != "NOT_A_MEM_OP" && !base.empty())
		{
			json &arr = per_array[base];
			if (arr.is_null())
			{
				arr = json::object();
				arr["current_bank_module"] = entry["current_bank_module"];
				arr["current_spm_base_addr"] = entry["current_spm_base_addr"];
				arr["memPEs_touched"] = json::array();
				arr["access_offsets"] = json::array();
				arr["ld_st_node_idxs"] = json::array();
			}
			if (!mapped_pe.is_null() && mapped_pe["is_memPE"])
			{
				json touched = mapped_pe;
				auto bit = memPE2Bank.find(node.rootDP->getPE());
				touched["bank"] = (bit != memPE2Bank.end())
									  ? canonicalBankKey(bit->second)
									  : std::string();
				arr["memPEs_touched"].push_back(touched);
			}
			if (observed_gep_offset >= 0)
				arr["access_offsets"].push_back(observed_gep_offset);
			arr["ld_st_node_idxs"].push_back(node.idx);
		}
	}

	out["arrays"] = json::object();
	for (auto &kv : per_array)
		out["arrays"][kv.first] = kv.second;

	ofstream f(fileName);
	f << out.dump(2) << "\n";
	f.close();
	std::cout << "[LayoutObservation] Wrote " << fileName << " ("
			  << out["load_store_nodes"].size() << " LD/ST nodes, "
			  << per_array.size() << " arrays)\n";
}

// ============================================================================
// EmitMappingStats: classifies DFG nodes by category, derives the scalar-
// equivalent ALU count from the arch JSON's PE grid + per-PE SIMD_WIDTH, and
// writes a per-phase stats JSON next to the mapping output.
//
// Op classification (heuristic, op-string based):
//   - load   : op startswith one of {LOAD, OLOAD, VLOAD, VOLOAD}
//   - store  : op startswith one of {STORE, OSTORE, VSTORE, VOSTORE}
//   - compute_vector / compute_scalar : op startswith a known arithmetic /
//             logic prefix (ADD, SUB, MUL, MAC, AND, OR, XOR, shifts,
//             comparisons, MOVC, SEXT, VRED, and their V-prefixed siblings).
//             simd_lanes > 1 -> compute_vector and contributes simd_lanes to
//             the scalar-equivalent count; otherwise compute_scalar contributes 1.
//   - iter_ctrl : everything else (CMERGE, SELECTPHI, BR, LOOPSTART, LOOPEXIT,
//             STORESTART, CTRLBrOR, NOP, ...). This is overhead from the
//             user's perspective.
//
// Limitations (see PLAN.md / REFERENCE.md):
//   - MAC counted as one op (one ALU-slot of work); does not double-count for
//     the fused multiply-add throughput.
//   - "iter_ctrl" is a catch-all; some control nodes are genuinely required
//     for predication, not just iteration counting. A backward slice from
//     real-store roots would be a more accurate "useful" split; deferred.
//   - stage_count_estimate uses max(ASAP)/II as a lower bound on the SWP
//     stage count; actual mapped latency may be higher.
//   - num_scalar_eq_ALUs is summed across all PE+PE_MEM grid entries; PE_DUMMY
//     is excluded. Pass --no-mem-alu in the future to exclude PE_MEM as well.
// ============================================================================
void CGRAXMLCompile::CGRA::EmitMappingStats(DFG *dfg, int II, const std::string &fileName)
{
	auto starts_with_any = [](const std::string &op, std::initializer_list<const char *> prefixes) {
		for (const char *p : prefixes) {
			std::string ps(p);
			if (op.rfind(ps, 0) == 0) return true;
		}
		return false;
	};
	auto is_load = [&](const std::string &op) {
		return starts_with_any(op, {"LOAD", "OLOAD", "VLOAD", "VOLOAD"});
	};
	auto is_store = [&](const std::string &op) {
		return starts_with_any(op, {"STORE", "OSTORE", "VSTORE", "VOSTORE"});
	};
	auto is_compute = [&](const std::string &op) {
		// Whitelist of arithmetic / logic / comparison ops counted as "real" compute.
		// MOVC / MOVCL / LOADCL move a constant into a register and are kept here
		// since they consume an ALU slot and produce a value used by data path nodes.
		return starts_with_any(op, {
			"ADD", "SUB", "MUL", "DIV", "MAC",
			"AND", "OR", "XOR", "LS", "RS", "ARS",
			"CMP", "CLT", "CGT", "PCMP", "NCMP",
			"SEXT", "MOVC", "MOVCL", "LOADCL",
			"VADD", "VSUB", "VMUL", "VDIV", "VMAC",
			"VAND", "VOR", "VXOR", "VLS", "VRS", "VARS",
			"VCMP", "VCLT", "VCGT", "VSEXT", "VRED"
		});
	};

	int n_load = 0, n_store = 0, n_compute_vec = 0, n_compute_scalar = 0, n_iter_ctrl = 0;
	long long scalar_eq_compute = 0;
	int max_asap = 0;
	std::map<int, int> simd_lanes_hist;
	std::map<std::string, int> per_op_count;

	for (DFGNode &node : dfg->nodeList) {
		if (node.ASAP > max_asap) max_asap = node.ASAP;
		int lanes = node.simd_lanes > 0 ? node.simd_lanes : 1;
		simd_lanes_hist[lanes]++;
		per_op_count[node.op]++;

		if (is_load(node.op)) {
			n_load++;
		} else if (is_store(node.op)) {
			n_store++;
		} else if (is_compute(node.op)) {
			if (lanes > 1) {
				n_compute_vec++;
				scalar_eq_compute += lanes;
			} else {
				n_compute_scalar++;
				scalar_eq_compute += 1;
			}
		} else {
			n_iter_ctrl++;
		}
	}

	// Walk the arch JSON to extract per-PE-type SIMD_WIDTH and grid counts.
	int pe_simd_width = 1, pe_mem_simd_width = 1;
	int n_pe = 0, n_pe_mem = 0, n_pe_dummy = 0, n_other_grid = 0;
	if (top_desc && top_desc->contains("ARCH")) {
		const json &arch = (*top_desc)["ARCH"];
		if (arch.contains("PE") && arch["PE"].contains("SIMD_WIDTH"))
			pe_simd_width = arch["PE"]["SIMD_WIDTH"].get<int>();
		if (arch.contains("PE_MEM") && arch["PE_MEM"].contains("SIMD_WIDTH"))
			pe_mem_simd_width = arch["PE_MEM"]["SIMD_WIDTH"].get<int>();

		if (arch.contains("CGRA") && arch["CGRA"].contains("SUBMODS")) {
			const json &submods = arch["CGRA"]["SUBMODS"];
			// After PreprocessPattern (CGRA.cpp:867), SUBMODS is rewritten from
			//   [{PATTERN: GRID, DIMS, MODS: [{X,Y,MOD}, ...]}]
			// to a dict keyed by mod type name:
			//   {PE: [...], PE_MEM: [...], PE_DUMMY: [...]}
			// We handle both forms in case future arches skip preprocessing.
			if (submods.is_object()) {
				for (auto it = submods.begin(); it != submods.end(); ++it) {
					const std::string &mt = it.key();
					if (!it.value().is_array()) continue;
					const int cnt = static_cast<int>(it.value().size());
					if (mt == "PE") n_pe += cnt;
					else if (mt == "PE_MEM") n_pe_mem += cnt;
					else if (mt == "PE_DUMMY") n_pe_dummy += cnt;
					else n_other_grid += cnt;
				}
			} else if (submods.is_array()) {
				for (const auto &sm : submods) {
					if (!sm.contains("MODS")) continue;
					for (const auto &mod : sm["MODS"]) {
						if (!mod.contains("MOD")) continue;
						const std::string mt = mod["MOD"].get<std::string>();
						if (mt == "PE") n_pe++;
						else if (mt == "PE_MEM") n_pe_mem++;
						else if (mt == "PE_DUMMY") n_pe_dummy++;
						else n_other_grid++;
					}
				}
			}
		}
	}

	const int num_scalar_eq_ALUs = n_pe * pe_simd_width + n_pe_mem * pe_mem_simd_width;
	const int schedule_length_kernel = II > 0 ? II : (max_asap + 1);
	// SWP stage count: derive from DFG critical path. After modulo scheduling
	// at II, a node at ASAP a sits in stage (a / II). Total stages
	// = floor(max_asap / II) + 1. This is a lower bound; the actual mapped
	// schedule may push some nodes later, growing N. Refining requires reading
	// per-node mapped latency, which is not directly exposed today.
	const int stage_count_estimate = (II > 0) ? (max_asap / II + 1) : 1;
	const int swp_fill_cycles = (II > 0 && stage_count_estimate > 1)
									? (stage_count_estimate - 1) * II : 0;
	const int swp_drain_cycles = swp_fill_cycles;

	double util_phase = 0.0;
	if (num_scalar_eq_ALUs > 0 && II > 0) {
		util_phase = static_cast<double>(scalar_eq_compute) /
					 (static_cast<double>(num_scalar_eq_ALUs) * II);
	}

	json out;
	out["phase"] = "kernel";  // overridden once DFG-gen emits explicit phase tags
	out["arch_name"] = getCGRAName();
	out["II"] = II;
	out["schedule_length_phase"] = schedule_length_kernel;
	out["dfg_critical_path"] = max_asap + 1;
	out["stage_count_estimate"] = stage_count_estimate;
	out["swp_fill_cycles"] = swp_fill_cycles;
	out["swp_drain_cycles"] = swp_drain_cycles;
	out["node_count"] = static_cast<int>(dfg->nodeList.size());

	out["grid"] = {
		{"num_PE", n_pe},
		{"num_PE_MEM", n_pe_mem},
		{"num_PE_DUMMY", n_pe_dummy},
		{"num_other_grid", n_other_grid},
		{"PE_simd_width", pe_simd_width},
		{"PE_MEM_simd_width", pe_mem_simd_width},
	};
	out["num_scalar_eq_ALUs"] = num_scalar_eq_ALUs;

	out["op_counts"] = {
		{"compute_vector", n_compute_vec},
		{"compute_scalar", n_compute_scalar},
		{"load", n_load},
		{"store", n_store},
		{"iter_ctrl", n_iter_ctrl},
	};
	out["scalar_eq_compute_ops"] = scalar_eq_compute;
	out["util_phase"] = util_phase;

	json hist = json::object();
	for (auto &kv : simd_lanes_hist) hist[std::to_string(kv.first)] = kv.second;
	out["simd_lanes_histogram"] = hist;

	json ops_breakdown = json::object();
	for (auto &kv : per_op_count) ops_breakdown[kv.first] = kv.second;
	out["per_op_count"] = ops_breakdown;

	std::ofstream ofs(fileName);
	ofs << out.dump(2) << "\n";
	ofs.close();
	std::cout << "[MappingStats] Wrote " << fileName
			  << " (II=" << II
			  << ", scalar_eq_compute=" << scalar_eq_compute
			  << ", num_scalar_eq_ALUs=" << num_scalar_eq_ALUs
			  << ", util=" << util_phase << ")\n";
}

namespace {

// Affine pattern for one LD/ST: addr = base_offset + iv * stride.
// stride = -1 means "unknown / no affine chain" (scalar or out-of-loop access).
struct AffinePattern {
	int base_offset = 0;
	int stride = -1;
	int iv_extern_idx = -1;
};

// Walk a *_AGI_subgraph.xml emitted by removeAGINodes and recover one
// AffinePattern per compute-side LD/ST that the AGI tree drives. The compute
// LD/ST is identified by the externalIdx attribute on AGI <Output> elements.
// The parser accepts a narrow linear form:
//   addr = constant + coefficient * one_external_index
// through ADD/GEPADD, GEPLEFTSHIFT, GEPSCALE, and constant MUL/GEPMUL nodes.
// Multi-index or nonlinear expressions are rejected rather than guessed.
std::unordered_map<int, AffinePattern> parseAGISubgraph(const std::string &path)
{
	std::unordered_map<int, AffinePattern> result;
	tinyxml2::XMLDocument doc;
	if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
		return result;
	tinyxml2::XMLElement *root = doc.FirstChildElement("AGI_DFG");
	if (!root)
		return result;

	struct AgiNode {
		std::string op;
		int constVal = 0;
		bool hasConst = false;
		std::vector<std::pair<int, bool>> inputs;  // (idx, isLocal)
		std::vector<std::pair<int, bool>> outputs;
	};
	std::map<int, AgiNode> nodes;
	for (tinyxml2::XMLElement *n = root->FirstChildElement("Node"); n; n = n->NextSiblingElement("Node")) {
		int localIdx = -1;
		if (n->QueryIntAttribute("localIdx", &localIdx) != tinyxml2::XML_SUCCESS)
			continue;
		AgiNode an;
		const char *opText = nullptr;
		if (tinyxml2::XMLElement *opE = n->FirstChildElement("OP"))
			opText = opE->GetText();
		if (opText) an.op = opText;
		int cv = 0;
		if (n->QueryIntAttribute("CONST", &cv) == tinyxml2::XML_SUCCESS) {
			an.constVal = cv;
			an.hasConst = true;
		}
		auto loadRefs = [&](const char *container, const char *ref,
							std::vector<std::pair<int, bool>> &out) {
			tinyxml2::XMLElement *c = n->FirstChildElement(container);
			if (!c) return;
			for (tinyxml2::XMLElement *r = c->FirstChildElement(ref); r;
				 r = r->NextSiblingElement(ref)) {
				int v = 0;
				if (r->QueryIntAttribute("localIdx", &v) == tinyxml2::XML_SUCCESS)
					out.push_back({v, true});
				else if (r->QueryIntAttribute("externalIdx", &v) == tinyxml2::XML_SUCCESS)
					out.push_back({v, false});
			}
		};
		loadRefs("Inputs", "Input", an.inputs);
		loadRefs("Outputs", "Output", an.outputs);
		nodes[localIdx] = std::move(an);
	}

	struct LinearExpr {
		bool valid = true;
		int base = 0;
		int coeff = 0;
		int ext = -1;
	};

	auto combineExternal = [](LinearExpr &expr, int ext, int coeff) {
		if (coeff == 0)
			return;
		if (expr.ext < 0)
			expr.ext = ext;
		else if (expr.ext != ext)
			expr.valid = false;
		expr.coeff += coeff;
	};

	std::function<LinearExpr(int, std::unordered_set<int> &)> evalLocal =
		[&](int localIdx, std::unordered_set<int> &active) -> LinearExpr {
			auto it = nodes.find(localIdx);
			if (it == nodes.end())
				return LinearExpr{false, 0, 0, -1};
			if (active.count(localIdx))
				return LinearExpr{false, 0, 0, -1};
			active.insert(localIdx);

			const AgiNode &n = it->second;
			LinearExpr expr;
			if (n.op == "ADD" || n.op == "GEPADD") {
				expr.base = n.hasConst ? n.constVal : 0;
				for (auto &in : n.inputs) {
					if (in.second) {
						LinearExpr child = evalLocal(in.first, active);
						if (!child.valid) {
							expr.valid = false;
							break;
						}
						expr.base += child.base;
						combineExternal(expr, child.ext, child.coeff);
					} else {
						combineExternal(expr, in.first, 1);
					}
				}
			} else if (n.op == "GEPLEFTSHIFT" && n.hasConst) {
				int scale = 1 << n.constVal;
				LinearExpr child;
				bool sawInput = false;
				for (auto &in : n.inputs) {
					sawInput = true;
					LinearExpr term;
					if (in.second)
						term = evalLocal(in.first, active);
					else {
						term.ext = in.first;
						term.coeff = 1;
					}
					if (!term.valid) {
						child.valid = false;
						break;
					}
					child.base += term.base;
					combineExternal(child, term.ext, term.coeff);
				}
				if (!sawInput)
					child = LinearExpr{};
				expr.valid = child.valid;
				expr.base = child.base * scale;
				expr.coeff = child.coeff * scale;
				expr.ext = child.ext;
			} else if ((n.op == "GEPSCALE" || n.op == "MUL" || n.op == "GEPMUL") &&
					   n.hasConst) {
				LinearExpr child;
				bool sawInput = false;
				for (auto &in : n.inputs) {
					sawInput = true;
					LinearExpr term;
					if (in.second)
						term = evalLocal(in.first, active);
					else {
						term.ext = in.first;
						term.coeff = 1;
					}
					if (!term.valid) {
						child.valid = false;
						break;
					}
					child.base += term.base;
					combineExternal(child, term.ext, term.coeff);
				}
				if (!sawInput)
					child = LinearExpr{};
				expr.valid = child.valid;
				expr.base = child.base * n.constVal;
				expr.coeff = child.coeff * n.constVal;
				expr.ext = child.ext;
			} else if (n.hasConst && n.inputs.empty()) {
				expr.base = n.constVal;
			} else {
				expr.valid = false;
			}

			active.erase(localIdx);
			return expr;
		};

	for (const auto &kv : nodes) {
		const AgiNode &add = kv.second;
		// Expect a GEPADD at the boundary: AGI op encoded as "ADD" or
		// "GEPADD" with at least one externalIdx Output (the LD/ST).
		if (add.op != "GEPADD" && add.op != "ADD")
			continue;
		if (!add.hasConst)
			continue;
		int compute_ext = -1;
		for (auto &o : add.outputs) {
			if (!o.second) { compute_ext = o.first; break; }
		}
		if (compute_ext < 0)
			continue;

		LinearExpr expr;
		expr.base = add.constVal;
		for (auto &i : add.inputs) {
			LinearExpr term;
			if (i.second) {
				std::unordered_set<int> active;
				term = evalLocal(i.first, active);
			} else {
				term.ext = i.first;
				term.coeff = 1;
			}
			if (!term.valid) {
				expr.valid = false;
				break;
			}
			expr.base += term.base;
			combineExternal(expr, term.ext, term.coeff);
			if (!expr.valid)
				break;
		}

		if (expr.valid && expr.ext >= 0 && expr.coeff != 0) {
			AffinePattern pat;
			pat.base_offset = expr.base;
			pat.stride = expr.coeff;
			pat.iv_extern_idx = expr.ext;
			result[compute_ext] = pat;
		}
	}
	return result;
}

} // anonymous namespace

bool CGRAXMLCompile::CGRA::SolveLayout(DFG *dfg, const LayoutSolverOpts &opts,
									   const std::string &agiSubgraphPath,
									   const std::string &decisionPath)
{
	if (!opts.enable) return false;

	auto patterns = parseAGISubgraph(agiSubgraphPath);
	// The runner remaps AGI externalIdx values after SIMD packing / pruning so
	// these keys should refer to the mapper-side DFGNode.idx namespace.

	struct LdStInfo {
		int idx;
		std::string op;
		PE *pe;
		Module *bank;
		AffinePattern pat;
		bool has_pat;
		bool is_store;
	};
	auto isMemOpInclusive = [](const std::string &op) {
		static const std::vector<std::string> prefixes = {
			"LOAD", "STORE", "OLOAD", "OSTORE",
			"VLOAD", "VSTORE", "VOLOAD", "VOSTORE"
		};
		for (auto &p : prefixes)
			if (op.rfind(p, 0) == 0) return true;
		return false;
	};
	auto bankKey = [](Module *bank) { return canonicalBankKey(bank); };
	std::map<std::string, std::vector<LdStInfo>> perArray;
	for (DFGNode &n : dfg->nodeList) {
		if (!isMemOpInclusive(n.op)) continue;
		if (n.base_pointer_name.empty() || n.base_pointer_name == "NOT_A_MEM_OP")
			continue;
		PE *pe = (n.rootDP) ? n.rootDP->getPE() : nullptr;
		if (!pe) continue;
		auto bit = memPE2Bank.find(pe);
		Module *bank = (bit != memPE2Bank.end()) ? bit->second : nullptr;
		LdStInfo info;
		info.idx = n.idx;
		info.op = n.op;
		info.pe = pe;
		info.bank = bank;
		auto pit = patterns.find(n.idx);
		info.has_pat = (pit != patterns.end());
		if (info.has_pat) info.pat = pit->second;
		info.is_store = (n.op.find("STORE") != std::string::npos);
		perArray[n.base_pointer_name].push_back(info);
	}

	json decision;
	decision["kernel"] = getCGRAName();
	decision["replicate_data"] = opts.replicate_data;
	decision["arrays"] = json::object();
	decision["node_addr_delta"] = json::object();
	// Snapshot pre-solver addresses so we can compute per-LD/ST address deltas
	// for the trace remapper.
	std::unordered_map<std::string,int> origAddr;
	for (auto &kv : Variable2SPMAddr) origAddr[kv.first] = kv.second;
	bool mutated = false;

	// Per-bank flat-address range derived from existing preset Variable2SPMAddr.
	// The simulator treats the address space as flat, so each bank physically
	// occupies a contiguous chunk [bank_min, bank_top). We bump within that
	// chunk when reassigning. Banks with no preset entries can't be inferred
	// safely; we mark them as "unknown" and the per-array logic falls back to
	// preserving the preset placement when the target bank is unknown.
	// Keyed by canonical bank name (wrapper minus "-T<n>") so all time-step
	// instances of one physical bank share one entry.
	struct BankRange { int min_addr = INT_MAX; int top = 0; bool known = false; };
	std::unordered_map<std::string, BankRange> bankRange;
	for (auto &kv : Variable2SPMAddr) {
		auto vit = Variable2SPM.find(kv.first);
		Module *b = (vit != Variable2SPM.end()) ? vit->second : nullptr;
		if (!b) continue;
		std::string key = bankKey(b);
		int sz = 0;
		auto pit = dfg->pointer_sizes.find(kv.first);
		if (pit != dfg->pointer_sizes.end()) sz = pit->second;
		auto &r = bankRange[key];
		r.known = true;
		if (kv.second < r.min_addr) r.min_addr = kv.second;
		int top = kv.second + std::max(sz, 1);
		if (top > r.top) r.top = top;
	}

	for (auto &kv : perArray) {
		const std::string &arr = kv.first;
		auto &lds = kv.second;
		json arrJ;
		arrJ["ld_st_nodes"] = json::array();
		// Dedup banks across time-replicated wrapper instances.
		std::map<std::string, Module*> bankByKey;
		bool any_store = false;
		bool any_pat = false;
		for (auto &li : lds) {
			json nodeJ = {{"idx", li.idx}, {"op", li.op}};
			if (li.has_pat) {
				nodeJ["affine"] = {
					{"base_offset", li.pat.base_offset},
					{"stride", li.pat.stride},
					{"iv_external_idx", li.pat.iv_extern_idx},
				};
			}
			arrJ["ld_st_nodes"].push_back(nodeJ);
			if (li.bank) {
				std::string k = bankKey(li.bank);
				if (!bankByKey.count(k)) bankByKey[k] = li.bank;
			}
			if (li.is_store) any_store = true;
			if (li.has_pat) any_pat = true;
		}
		arrJ["banks"] = json::array();
		for (auto &bk : bankByKey) arrJ["banks"].push_back(bk.first);
		size_t banks_count = bankByKey.size();

		int arr_size = 0;
		auto sit = dfg->pointer_sizes.find(arr);
		if (sit != dfg->pointer_sizes.end()) arr_size = sit->second;

		auto assignBank = [&](Module *bank, const std::string &varName, int size) -> int {
			// Returns -1 if the bank's flat-address range is unknown (no preset
			// entries to anchor on); caller treats -1 as "skip this mutation".
			std::string key = bankKey(bank);
			auto rit = bankRange.find(key);
			if (rit == bankRange.end() || !rit->second.known)
				return -1;
			BankRange &r = rit->second;
			int addr = r.top;
			r.top = addr + std::max(size, 1);
			Variable2SPM[varName] = bank;
			Variable2SPMAddr[varName] = addr;
			return addr;
		};

		if (banks_count == 1) {
			Module *target = bankByKey.begin()->second;
			std::string targetKey = bankByKey.begin()->first;
			auto cur = Variable2SPM.find(arr);
			std::string curKey = (cur != Variable2SPM.end()) ? bankKey(cur->second) : std::string();
			if (cur == Variable2SPM.end() || curKey != targetKey) {
				int orig = origAddr.count(arr) ? origAddr[arr] : 0;
				int addr = assignBank(target, arr, arr_size > 0 ? arr_size : 1);
				if (addr < 0) {
					arrJ["strategy"] = "fallback_unknown_bank_range";
					arrJ["reason"] = "target bank has no preset entries; can't pick flat address";
					std::cerr << "[LayoutSolver] array '" << arr
							  << "' fallback: target bank's flat-address range unknown\n";
				} else {
					arrJ["strategy"] = "consolidate";
					arrJ["bank"] = target->getName();
					arrJ["base_addr"] = addr;
					arrJ["orig_base_addr"] = orig;
					int delta = addr - orig;
					for (auto &li : lds)
						decision["node_addr_delta"][std::to_string(li.idx)] = delta;
					mutated = true;
				}
			} else {
				arrJ["strategy"] = "already_consolidated";
				arrJ["bank"] = target->getName();
				arrJ["base_addr"] = Variable2SPMAddr[arr];
			}
		} else if (banks_count > 1 && any_pat && !any_store) {
			// Tentative partition: each bank gets a partition variable
			// "<arr>_part<i>" placed at a fresh local address. Strides
			// must agree across LD/STs (cyclic partition). If patterns
			// disagree, fall back.
			int common_stride = -1;
			bool ok = true;
			for (auto &li : lds) {
				if (!li.has_pat) { ok = false; break; }
				if (common_stride < 0) common_stride = li.pat.stride;
				else if (common_stride != li.pat.stride) { ok = false; break; }
			}
			if (ok && opts.replicate_data) {
				int orig_addr = Variable2SPMAddr.count(arr) ? Variable2SPMAddr[arr] : 0;
				int part_idx = 0;
				std::unordered_map<std::string, int> replicaAddrByKey;
				bool any_bank_unknown = false;
				for (auto &bk : bankByKey) {
					std::string partName = arr + "_repl" + std::to_string(part_idx++);
					int addr = assignBank(bk.second, partName, arr_size > 0 ? arr_size : 1);
					if (addr < 0) { any_bank_unknown = true; break; }
					replicaAddrByKey[bk.first] = addr;
					arrJ["per_bank_replicas"][bk.first] = {{"var", partName}, {"base_addr", addr}};
				}
				if (any_bank_unknown) {
					arrJ["strategy"] = "fallback_unknown_bank_range";
					arrJ["reason"] = "one or more target banks have no preset entries";
					std::cerr << "[LayoutSolver] array '" << arr
							  << "' replicate fallback: a target bank's flat-address range unknown\n";
				} else {
					arrJ["strategy"] = "replicate";
					arrJ["per_lane"] = json::object();
					for (auto &li : lds) {
						int addr = replicaAddrByKey[bankKey(li.bank)];
						perNodeAddrOverride[li.idx] = addr;
						decision["node_addr_delta"][std::to_string(li.idx)] = addr - orig_addr;
					}
					arrJ["orig_base_addr"] = orig_addr;
					mutated = true;
				}
			} else if (ok) {
				// Cyclic partition by per-bank residue class.
				int orig_addr = origAddr.count(arr) ? origAddr[arr] : 0;
				int part_idx = 0;
				std::unordered_map<std::string, std::pair<int,std::string>> partInfoByKey;
				bool any_bank_unknown = false;
				for (auto &bk : bankByKey) {
					std::string partName = arr + "_part" + std::to_string(part_idx++);
					int part_size = std::max(1, (arr_size + (int)banks_count - 1) / (int)banks_count);
					int addr = assignBank(bk.second, partName, part_size);
					if (addr < 0) { any_bank_unknown = true; break; }
					partInfoByKey[bk.first] = {addr, partName};
					arrJ["per_bank_partitions"][bk.first] = {
						{"var", partName}, {"base_addr", addr}, {"part_size", part_size}};
				}
				if (any_bank_unknown) {
					arrJ["strategy"] = "fallback_unknown_bank_range";
					arrJ["reason"] = "one or more partition target banks have no preset entries";
					std::cerr << "[LayoutSolver] array '" << arr
							  << "' partition fallback: a target bank's flat-address range unknown\n";
				} else {
					arrJ["strategy"] = "partition";
					arrJ["stride"] = common_stride;
					for (auto &li : lds) {
						int addr = partInfoByKey[bankKey(li.bank)].first;
						perNodeAddrOverride[li.idx] = addr;
						arrJ["per_node_addr"][std::to_string(li.idx)] = addr;
						decision["node_addr_delta"][std::to_string(li.idx)] = addr - orig_addr;
					}
					arrJ["orig_base_addr"] = orig_addr;
					mutated = true;
				}
			} else {
				arrJ["strategy"] = "fallback_no_clean_pattern";
				arrJ["reason"] = "LD/STs of '" + arr + "' span banks but affine patterns disagree";
				std::cerr << "[LayoutSolver] " << arrJ["reason"].get<std::string>() << "\n";
			}
		} else if (banks_count > 1) {
			arrJ["strategy"] = "fallback_conflict";
			std::string why = any_store
				? "RW array spans banks; replication unsafe"
				: "missing affine pattern info";
			arrJ["reason"] = why;
			std::cerr << "[LayoutSolver] array '" << arr << "' fallback: " << why << "\n";
		} else {
			arrJ["strategy"] = "untouched";
		}
		decision["arrays"][arr] = arrJ;
	}

	std::ofstream df(decisionPath);
	df << decision.dump(2) << "\n";
	df.close();
	std::cout << "[LayoutSolver] Wrote " << decisionPath
			  << " (mutated=" << (mutated ? "true" : "false") << ")\n";
	return mutated;
}

void CGRAXMLCompile::CGRA::WriteMemAllocTxt(const std::string &fileName)
{
	std::ofstream f(fileName);
	f << "var_name,base_addr\n";
	for (auto &kv : Variable2SPMAddr) {
		f << kv.first << "," << kv.second << "\n";
	}
	f.close();
	std::cout << "[LayoutSolver] Wrote " << fileName << "\n";
}

// by Yujie
void CGRAXMLCompile::CGRA::PrintMappedPillarsModule(Module *curr_module, json &output_json, ofstream& outFile_i)
{
	output_json["TYPE"] = curr_module->get_type();
	curr_module->UpdateMappedConnectionsPillars(output_json, outFile_i);

	for (Module *sub_module : curr_module->subModules)
	{
		PrintMappedPillarsModule(sub_module, output_json["SUBMODS"][sub_module->getName()], outFile_i);
	}
}

//by Yujie
// get multiplexer name from destination port name
string CGRAXMLCompile::CGRA::getMuxName(string src_port_name, string desc_port_name, int* inputID){
	std::map<string, string>::iterator Desc_iter;
	string descName;
	Desc_iter = Desc2Mux.find(desc_port_name);
	if(Desc_iter != Desc2Mux.end()){
		descName = Desc_iter->second;
	}else{
		interConnection = true;
		return descName;
	}
	// string descName = Desc2Mux.find(desc_port_name)->second;
	// std::cout<<descName<<std::endl;	
	std::map<string, std::map<string, int>>::iterator iter;
	iter = SourcePort.find(descName);
	if(iter != SourcePort.end()){
		std::map<string, int> component = iter->second;
		std::map<string, int>::iterator it = component.find(src_port_name);
		if(it != component.end()){
			*inputID = it->second;
		}else{
			interConnection = true;
			return descName;
		}
	}else{
		LOG(PILLARS)<<"!!!!!!!!!!!!!!Wrong!!!!!!!!!!!!"<<"\n";
	}
	return descName;
}

string CGRAXMLCompile::CGRA::getFUName(string operations, int* output_ID){
	std::map<string, int>::iterator Op_iter;
	Op_iter = Op2ID.find(operations);
	if(Op_iter != Op2ID.end()){
		*output_ID = Op_iter->second;
	}else{
		interConnection = true; // to judge whether needs to add suffix to op
		LOG(PILLARS)<<"!!!!!!!!!!!!!!Wrong!!!!!!!!!!!!"<<"\n";
	}
	if(*output_ID==18||*output_ID==19||(*output_ID>=27 && *output_ID<=32)){
		return "loadStoreUnit";
	}
	return "alu0";
}

// by Yujie
int CGRAXMLCompile::CGRA::getRegInfo(string src_port_name, string dest_port_name, int* input_ID, int* output_ID){
	int regID = 0;
	if(dest_port_name.compare("RP0")==0){
		*output_ID = 0;
		*input_ID = -1;
		regID = (int)src_port_name[src_port_name.length()-4] - 48;
	}else if(dest_port_name.compare("RP1")==0){
		*output_ID = 1;
		*input_ID = -1;
		regID = (int)src_port_name[src_port_name.length()-4] - 48;		
	}else{
		*output_ID = -1;
		regID = (int)dest_port_name[dest_port_name.length()-4] - 48;			
		if(src_port_name.compare("WP0")==0){
			*input_ID = 0;
		}else{
			*input_ID = 1;
		}
	}
	return regID;
}

// by Yujie
void CGRAXMLCompile::Module::UpdateMappedConnectionsPillars(json &output_json, ofstream& outFile_i)
{
	Module *mod = this;
	CGRA *cgra = getCGRA();
	do
	{
		int t = mod->get_t();
		PE* mod_pe =mod->getPE();
		int Y = mod_pe->getPosition_Y();
		int X = mod_pe->getPosition_X();

		for (auto it = mod->connectedTo.begin(); it != mod->connectedTo.end(); it++)
		{
			Port *src_port = it->first;
			Module *src_module = src_port->getMod();
			string src_port_name;
			src_port_name = src_port->getName();

			//if source port is not mapped no need to explore
			if (src_port->getNode() == NULL)
			{
				// cout << "src_port = " << src_port->getFullName() << "is not used!\n";
				continue;
			}

			for (Port *dest_port : it->second)
			{
				Module *dest_module = dest_port->getMod();
				string dest_port_name;
				dest_port_name = dest_port->getName();

				//the dest port is not in use
				if (dest_port->getNode() == NULL)
					continue;

				int LatencyDiff = dest_port->getLat() - src_port->getLat();
				bool isTemproalRegConnection = src_port->getType() == REGO && dest_port->getType() == REGI;
				bool isBothRegPorts = (src_port->getType() == REGO && dest_port->getType() == REGI) || (src_port->getType() == REGI && dest_port->getType() == REGO);
				bool hasSameNode = dest_port->getNode() == src_port->getNode();
				bool valid_connection = false;

				if (!isBothRegPorts)
				{
					if (LatencyDiff == 0 && hasSameNode)
					{
						// to do !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
						output_json["CONNECTIONS"][to_string(t)][src_port_name].push_back(dest_port_name);
						string ifRF = "RF";
						string mux_desc = "!!!!!!!!!!!!!!!!!!Connection!!!!!!!!!!!!!!!!!!!!!!!!!!!";
						int input_ID = 0;
						int output_ID = 0;
						int regID = 0;
						// std::cout<<src_port_name<<","<<dest_port_name<<std::endl;
						if(ifRF.compare(mod->get_type())==0){
							// for rf0
							regID = cgra->getRegInfo(src_port_name, dest_port_name, &input_ID, &output_ID);
							outFile_i<<"<"<<t<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<".rf0.internalNode_"<<regID<<">"<<std::endl;							
							outFile_i<<input_ID<<std::endl << output_ID << std::endl;
							// std::cout<<"<"<<t<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<".rf0.internalNode_"<<regID<<">"<<std::endl;							
							// std::cout<<input_ID<<std::endl << output_ID << std::endl;
						}else{
							mux_desc = cgra->getMuxName(src_port_name, dest_port_name, &input_ID);
							if(cgra->interConnection){
								cgra->interConnection = false;
							}else{
								outFile_i<<"<"<<t<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<"."<<mux_desc<<".internalNode_0>"<<std::endl;
								outFile_i<<input_ID<<std::endl << 0 << std::endl;
								// std::cout<<"<"<<t<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<"."<<mux_desc<<".internalNode_0>"<<std::endl;
								// std::cout<<input_ID<<std::endl << 0 << std::endl;
							}
						}
						output_json["MAPPED_NODES"][to_string(t)][src_port_name] = src_port->getNode()->idx;
						output_json["MAPPED_NODES"][to_string(t)][dest_port_name] = dest_port->getNode()->idx;
						valid_connection = true;
					}
				}
			}
		}
		mod = mod->getNextTimeIns();
	} while (mod != this);

	//If this is a funcional unit explore children for datapath submodule
	if (FU *this_fu = dynamic_cast<FU *>(this))
	{
		for (Module *sub_module : this_fu->subModules)
		{
			if (DataPath *dp = dynamic_cast<DataPath *>(sub_module))
			{
				Module *mod = dp;
				do
				{
					int t = mod->get_t();
					int II = cgra->get_t_max();
					PE* mod_pe =mod->getPE();
					int Y = mod_pe->getPosition_Y();
					int X = mod_pe->getPosition_X();

					DataPath *mod_dp = static_cast<DataPath *>(mod);
					if (mod_dp->getMappedNode())
					{
						// to do !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
						output_json["OPS"][to_string(t)] = mod_dp->getMappedNode()->op;

						string mux_desc = "!!!!!!!!!!!!!!!FU!!!!!!!!!!";
						int output_ID = 0;
						string suffix_op = "";
						if(mod_dp->getMappedNode()->npb){
							suffix_op = "_NPB";
						}
						if(mod_dp->getMappedNode()->hasConst){
							suffix_op = "_CONST";
						}
						mux_desc = cgra->getFUName(mod_dp->getMappedNode()->op+suffix_op, &output_ID);
						LOG(PILLARS)<< mod_dp->getMappedNode()->op+suffix_op <<"\n";

						if(cgra->interConnection){
							cgra->interConnection = false;
							mux_desc = cgra->getFUName(mod_dp->getMappedNode()->op, &output_ID);
							LOG(PILLARS)<< mod_dp->getMappedNode()->op<<"\n";
						}
						
						outFile_i<<"<"<<(t+1)%II<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<"."<<mux_desc<<".internalNode_0>"<<std::endl;
						outFile_i<<"SELECTED_OP"<<std::endl;
						outFile_i <<output_ID<< std::endl;						

						// std::cout<<"<"<<(t+1)%cgra->get_t_max()<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<"."<<mux_desc<<".internalNode_0>"<<std::endl;
						// std::cout<<"SELECTED_OP"<<std::endl;
						// std::cout <<output_ID<< std::endl;	

						if(mux_desc.compare("loadStoreUnit")==0){
							outFile_i<<"<"<<(t+2)%II<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<".muxT.internalNode_0>"<<std::endl;
							outFile_i<<0<<std::endl<<0<<std::endl;
							// std::cout<<"<"<<(t+2)%cgra->get_t_max()<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<".muxT.internalNode_0>"<<std::endl;
							// std::cout<<0<<std::endl<<0<<std::endl;
						}

						output_json["OP_INFO"][to_string(t)]["id"] = mod_dp->getMappedNode()->idx;
						output_json["OP_INFO"][to_string(t)]["NPB"] = mod_dp->getMappedNode()->npb;

						if(mod_dp->getMappedNode()->hasConst){
							//if there are constants they are always fed to I2 port.
							output_json["CONNECTIONS"][to_string(t)]["CONST." + to_string(mod_dp->getMappedNode()->constant)] = mod_dp->getName() + ".I2";
							outFile_i<<"<"<<(t+1)%II<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<".const0.internalNode_0>"<<std::endl;
							outFile_i<<"SELECTED_OP"<<std::endl<<7<<std::endl;
							// std::cout<<"<"<<(t+1)%cgra->get_t_max()<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<".const0.internalNode_0>"<<std::endl;
							// std::cout<<"SELECTED_OP"<<std::endl<<7<<std::endl;
							cgra->insertConstOp(mod_dp->getMappedNode()->idx, (t+1)%II, Y, X);
							// outFile_r<<"const"<<mod_dp->getMappedNode()->idx<<" "<<(t+1)%II<<":cgra.tile_0.pe_"<<Y<<"_"<<X<<".const0.internalNode_0"<<" "<<(t+1)%II<<" 0"<<std::endl;
						}

					}
					mod = mod->getNextTimeIns();
				} while (mod != dp);

				//Assumption :: only 1 datapath module is found inside FU
				break;
			}
		}
	}
}

void CGRAXMLCompile::CGRA::PrintMappedJSONModule(Module *curr_module, json &output_json)
{
	output_json["TYPE"] = curr_module->get_type();
	curr_module->UpdateMappedConnectionsJSON(output_json);

	for (Module *sub_module : curr_module->subModules)
	{
		PrintMappedJSONModule(sub_module, output_json["SUBMODS"][sub_module->getName()]);
	}
}

void CGRAXMLCompile::CGRA::SearchALLSPMs(Module *currModule, unordered_set<Module *> &spms)
{
	// cout << "SearchALLSPMs :: currmod = " << currModule->getFullName() << "\n";
	if (currModule->get_type() == "SPM")
	{
		spms.insert(currModule);
	}

	if (CGRA *cgra_ins = dynamic_cast<CGRA *>(currModule))
	{
		for (Module *submod : cgra_ins->subModArr[0])
		{
			SearchALLSPMs(submod, spms);
		}
	}
	else
	{
		for (Module *submod : currModule->subModules)
		{
			SearchALLSPMs(submod, spms);
		}
	}
}

void GetLastTSockets(Module *curr, Port *connecting_port, unordered_set<Port *> &last_tsockets)
{
	// cout << "GetLastTSockets :: currmod = " << curr->getFullName() << ", cp = " << connecting_port->getFullName() << "\n";
	assert(connecting_port->getType() == SOCKET);

	bool another_level_exist = false;
	if (curr->getParent())
	{
		vector<Port *> nPorts = curr->getParent()->getNextPorts(connecting_port);
		for (Port *np : nPorts)
		{
			assert(np->getType() == SOCKET);
			if (np->getMod() && curr->getParent() && np->getMod() == curr->getParent())
			{
				another_level_exist = true;
				GetLastTSockets(curr->getParent(), np, last_tsockets);
			}
		}
	}

	if (!another_level_exist)
	{
		last_tsockets.insert(connecting_port);
		return;
	}
}

void GetEndMPDs(Module *curr, Port *connecting_isocket, unordered_set<DataPath *> &end_datapaths)
{
	// cout << "GetEndMPDs :: currmod = " << curr->getFullName() << ", isocket = " << connecting_isocket->getFullName() << "\n";
	assert(connecting_isocket->getType() == SOCKET);

	if (DataPath *mdp = dynamic_cast<DataPath *>(curr))
	{
		end_datapaths.insert(mdp);
	}
	else
	{
		vector<Port *> fromPorts = curr->getFromPorts(connecting_isocket);
		for (Port *fp : fromPorts)
		{
			assert(fp->getType() == SOCKET);
			if (fp->getMod()->getParent() == curr)
			{
				GetEndMPDs(fp->getMod(), fp, end_datapaths);
			}
		}
	}
}

void CGRAXMLCompile::CGRA::checkMDPVars(unordered_set<Module *> &spms)
{
	LOG(CGRA) << "checkMDPVars started.\n";
	// unordered_set<Module *> spms;
	// SearchALLSPMs(this, spms);

	// assert(!spms.empty());

	for (Module *spm : spms)
	{
		LOG(CGRA) << "spm = " << spm->getFullName() << "\n";
		unordered_set<Port *> last_tsockets;
		for (Port *sp : spm->socketPorts)
		{
			GetLastTSockets(spm, sp, last_tsockets);
		}

		assert(!last_tsockets.empty());

		unordered_set<DataPath *> end_datapaths;
		for (Port *ts : last_tsockets)
		{
			LOG(CGRA) << "last tsocket = " << ts->getFullName() << "\n";
			vector<Port *> next_isockets = connectedFrom[ts];
			for (Port *is : next_isockets)
			{
				LOG(CGRA) << "\t next isocket = " << is->getFullName() << "\n";
				assert(is->getType() == SOCKET);
				GetEndMPDs(is->getMod(), is, end_datapaths);
			}
		}

		assert(spm->isSPM);
		for (string memvar : spm->data_layout)
		{
			LOG(CGRA) << "memvar = " << memvar << "\n";
			for (DataPath *dp : end_datapaths)
			{
				unordered_set<DataPath*> all_t_end_datapaths;
				DataPath* next_t_dp = dp;
				do{
					all_t_end_datapaths.insert(next_t_dp);
					next_t_dp = static_cast<DataPath*>(next_t_dp->getNextTimeIns());
				}while(next_t_dp != dp);

				for(DataPath* tdp : all_t_end_datapaths){
					LOG(CGRA) << "\t dp = " << tdp->getFullName() << "\n";
					tdp->accesible_memvars.insert(memvar);
				}
			}
		}
	}

	LOG(CGRA) << "checkMDPVars done.\n";
	// assert(false);
}

void CGRAXMLCompile::CGRA::InsertVariable2SPMAddrInfo(json &output_json)
{

	for (auto it = Variable2SPMAddr.begin(); it != Variable2SPMAddr.end(); it++)
	{
		string var = it->first;
		int addr = it->second;
		Module *spm = Variable2SPM[var];

		output_json["DATA_LAYOUT"][var]["spm"] = spm->getFullName();
		output_json["DATA_LAYOUT"][var]["addr"] = addr;
	}
}

bool CGRAXMLCompile::CGRA::PreprocessInterSubmodConns(json &arch)
{

	unordered_set<string> sockets;

	for (auto &el : arch.items())
	{
		string mod_type = el.key();
		for (auto &socket_str : arch[mod_type]["ISOCKETS"])
			sockets.insert((mod_type + ":" + (string)socket_str));
		for (auto &socket_str : arch[mod_type]["TSOCKETS"])
			sockets.insert((mod_type + ":" + (string)socket_str));
		for (auto &socket_str : arch[mod_type]["SOCKETS"])
			sockets.insert((mod_type + ":" + (string)socket_str));
	}

	for (auto &el : arch.items())
	{
		string mod_type = el.key();
		if (mod_type == "CGRA")
			continue;

		struct Connection
		{
			string src_mod;
			string src_port;
			string dest_mod;
			string dest_port;
			Connection(string s1, string s2, string s3, string s4) : src_mod(s1), src_port(s2), dest_mod(s3), dest_port(s4) {}
		};

		vector<Connection> inter_submod_conns;

		unordered_map<string, string> insname2type;
		for (auto &submod : arch[mod_type]["SUBMODS"].items())
		{
			string type = submod.key();
			for (auto &ins : submod.value())
			{
				string name = ins["name"];
				insname2type[name] = type;
			}
		}

		for (auto &conn : arch[mod_type]["CONNECTIONS"].items())
		{
			string src = conn.key();
			string src_mod_str = src.substr(0, src.find("."));
			string src_port_str = src.erase(0, src.find(".") + 1);

			string src_mod_type;
			if (src_mod_str == "THIS")
			{
				src_mod_type = mod_type;
			}
			else
			{
				assert(insname2type.find(src_mod_str) != insname2type.end());
				src_mod_type = insname2type[src_mod_str];
			}
			// if(src_mod_type == "CGRA") continue;
			if (sockets.find(src_mod_type + ":" + src_port_str) != sockets.end())
				continue;

			for (string dest : conn.value())
			{
				string dest_mod_str = dest.substr(0, dest.find("."));
				string dest_port_str = dest.erase(0, dest.find(".") + 1);

				string dest_mod_type;
				if (dest_mod_str == "THIS")
				{
					dest_mod_type = mod_type;
				}
				else
				{
					assert(insname2type.find(dest_mod_str) != insname2type.end());
					dest_mod_type = insname2type[dest_mod_str];
				}
				// if(dest_mod_type == "CGRA") continue;
				if (sockets.find(dest_mod_type + ":" + dest_port_str) != sockets.end())
					continue;

				if (src_mod_str != "THIS" && dest_mod_str != "THIS")
				{
					inter_submod_conns.push_back(Connection(src_mod_str, src_port_str, dest_mod_str, dest_port_str));
				}
			}
		}

		//remove intra submodule connection and add internal proxy to connect those
		for (Connection &rconn : inter_submod_conns)
		{

			string old_src = rconn.src_mod + "." + rconn.src_port;
			string old_dest = rconn.dest_mod + "." + rconn.dest_port;

			auto erase_it = find(arch[mod_type]["CONNECTIONS"][old_src].begin(), arch[mod_type]["CONNECTIONS"][old_src].end(), old_dest);
			assert(erase_it != arch[mod_type]["CONNECTIONS"][old_src].end());
			arch[mod_type]["CONNECTIONS"][old_src].erase(erase_it);
			// string new_internal_port_name = rconn.src_mod + "_" + rconn.src_port + "-->" + rconn.dest_mod + "_" + rconn.dest_port;
			string new_internal_port_name = rconn.dest_port;

			arch[mod_type]["INTERNALS"].push_back(new_internal_port_name);
			arch[mod_type]["CONNECTIONS"][old_src].push_back("THIS." + new_internal_port_name);
			arch[mod_type]["CONNECTIONS"]["THIS." + new_internal_port_name].push_back(old_dest);
		}
	}
}
