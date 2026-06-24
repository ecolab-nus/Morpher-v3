#include "ArtifactSim/ArtifactSimulator.h"
#include "ArtifactSim/ArchSimulator.h"
#include "ArtifactSim/ArchSchedule.h"
#include "ArtifactSim/CGRAArch.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ArtifactSim;

struct PhaseArg {
    std::string dfg;
    std::string mapping;
    std::string memAlloc;
};

struct Args {
    std::string dfg;
    std::string mapping;
    std::string memAlloc;
    std::string trace;
    std::string addrTrace;
    std::string arch;    // optional: path to CGRA arch JSON
    std::string result;  // optional: path to mapper result.txt (for II)
    int xdim = 4;
    int ydim = 4;
    int maxIterations = 1024;
    int maxHops = 4;     // mapper --hops default for HyCUBE
    bool trustMapperRouting = false;
    std::vector<PhaseArg> extraPhases;
};

static void usage() {
    std::cerr
        << "Usage: artifact_simulator --dfg <xml> --mapping <mappingwithlatency.txt> "
        << "--mem-alloc <mem_alloc.txt> --trace <trace.txt> [--x <n>] [--y <n>] "
        << "[--max-iters <n>] [--addr-trace <addr_trace.txt>] "
        << "[--arch <arch.json>] [--result <result.txt>] "
        << "[--max-hops <n>] "
        << "[--trust-mapper-routing] "
        << "[--phase <dfg:mapping:mem_alloc>]...\n";
}

static PhaseArg parsePhaseSpec(const std::string &spec) {
    std::vector<std::string> parts;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ':')) {
        parts.push_back(item);
    }
    if (parts.size() != 3 || parts[0].empty() || parts[1].empty() || parts[2].empty()) {
        throw std::runtime_error(
            "invalid --phase; expected <dfg>:<mapping>:<mem_alloc>");
    }
    return PhaseArg{parts[0], parts[1], parts[2]};
}

static Args parseArgs(int argc, char **argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        auto needValue = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };
        if      (opt == "--dfg")       args.dfg           = needValue(opt);
        else if (opt == "--mapping")   args.mapping        = needValue(opt);
        else if (opt == "--mem-alloc") args.memAlloc       = needValue(opt);
        else if (opt == "--trace")     args.trace          = needValue(opt);
        else if (opt == "--addr-trace")args.addrTrace      = needValue(opt);
        else if (opt == "--arch")      args.arch           = needValue(opt);
        else if (opt == "--result")    args.result         = needValue(opt);
        else if (opt == "--phase")     args.extraPhases.push_back(parsePhaseSpec(needValue(opt)));
        else if (opt == "--x")         args.xdim           = std::stoi(needValue(opt));
        else if (opt == "--y")         args.ydim           = std::stoi(needValue(opt));
        else if (opt == "--max-iters") args.maxIterations  = std::stoi(needValue(opt));
        else if (opt == "--max-hops")  args.maxHops        = std::stoi(needValue(opt));
        else if (opt == "--trust-mapper-routing") args.trustMapperRouting = true;
        else if (opt == "--help") {
            usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + opt);
        }
    }
    if (args.dfg.empty() || args.mapping.empty()
        || args.memAlloc.empty() || args.trace.empty()) {
        usage();
        throw std::runtime_error("missing required arguments");
    }
    return args;
}

static int resolveII(const std::string &mappingPath, const std::string &resultPath) {
    int II = 0;
    if (!resultPath.empty()) {
        std::ifstream rf(resultPath);
        std::string line;
        while (std::getline(rf, line)) {
            if (line.find("status:success") == std::string::npos) continue;
            std::istringstream ss(line);
            std::string tok;
            int field = 0;
            while (ss >> tok) {
                if (field == 2) { II = std::stoi(tok); break; }
                ++field;
            }
            if (II > 0) break;
        }
    }
    if (II <= 0) {
        II = ArchSchedule::parseIIFromFilename(mappingPath);
    }
    return II > 0 ? II : 1;
}

int main(int argc, char **argv) {
    try {
        Args args = parseArgs(argc, argv);

        std::vector<PhaseArg> phases;
        phases.push_back(PhaseArg{args.dfg, args.mapping, args.memAlloc});
        phases.insert(phases.end(), args.extraPhases.begin(), args.extraPhases.end());

        DFGGraph graph   = DFGGraph::parse(phases.front().dfg);
        Mapping  mapping = Mapping::parse(phases.front().mapping);
        MemAlloc alloc   = parseMemAlloc(phases.front().memAlloc);
        TraceImage trace = parseTrace(args.trace, alloc);
        if (!args.addrTrace.empty()) {
            parseAddressTrace(args.addrTrace, trace);
        }

        SimOptions options;
        options.xdim          = args.xdim;
        options.ydim          = args.ydim;
        options.maxIterations = args.maxIterations;

        SimResult result;

        if (!args.arch.empty()) {
            // Arch-aware path: parse CGRA JSON, build mesh, validate schedule.
            CGRAArch arch = CGRAArch::parse(args.arch);
            std::map<std::pair<int, int>, Value> tregValues;

            for (size_t idx = 0; idx < phases.size(); ++idx) {
                const PhaseArg &phase = phases[idx];
                DFGGraph phaseGraph = DFGGraph::parse(phase.dfg);
                Mapping phaseMapping = Mapping::parse(phase.mapping);
                MemAlloc phaseAlloc = parseMemAlloc(phase.memAlloc);
                const int II = resolveII(
                    phase.mapping, idx == 0 ? args.result : std::string{});

                CGRAMesh mesh(arch);
                ArchSchedule schedule = ArchSchedule::build(
                    phaseMapping, mesh, phaseGraph, II,
                    args.trustMapperRouting ? 1000000 : args.maxHops);

                ArchSimulator sim(CGRAArch(arch), std::move(schedule),
                                  std::move(phaseGraph), std::move(phaseMapping),
                                  std::move(phaseAlloc), trace, options);
                result = sim.runWithState(tregValues, idx + 1 == phases.size());
                trace = sim.traceImage();
            }
        } else {
            if (phases.size() != 1) {
                throw std::runtime_error("--phase requires --arch so PE-local TREG state is defined");
            }
            // Original arch-unaware path (unchanged behaviour).
            Simulator sim(std::move(graph), std::move(mapping),
                          std::move(alloc), std::move(trace), options);
            result = sim.run();
        }

        std::cout << "Simulation Result: Matches::" << result.matches
                  << ", Mismatches::" << result.mismatches << "\n";
        return result.mismatches == 0 ? 0 : 2;

    } catch (const std::exception &exc) {
        std::cerr << "Error: " << exc.what() << "\n";
        return 1;
    }
}
