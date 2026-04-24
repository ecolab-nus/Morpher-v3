#include "ArtifactSim/ArtifactSimulator.h"
#include "ArtifactSim/ArchSimulator.h"
#include "ArtifactSim/ArchSchedule.h"
#include "ArtifactSim/CGRAArch.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace ArtifactSim;

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
};

static void usage() {
    std::cerr
        << "Usage: artifact_simulator --dfg <xml> --mapping <mappingwithlatency.txt> "
        << "--mem-alloc <mem_alloc.txt> --trace <trace.txt> [--x <n>] [--y <n>] "
        << "[--max-iters <n>] [--addr-trace <addr_trace.txt>] "
        << "[--arch <arch.json>] [--result <result.txt>] "
        << "[--max-hops <n>]\n";
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
        else if (opt == "--x")         args.xdim           = std::stoi(needValue(opt));
        else if (opt == "--y")         args.ydim           = std::stoi(needValue(opt));
        else if (opt == "--max-iters") args.maxIterations  = std::stoi(needValue(opt));
        else if (opt == "--max-hops")  args.maxHops        = std::stoi(needValue(opt));
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

int main(int argc, char **argv) {
    try {
        Args args = parseArgs(argc, argv);

        DFGGraph graph   = DFGGraph::parse(args.dfg);
        Mapping  mapping = Mapping::parse(args.mapping);
        MemAlloc alloc   = parseMemAlloc(args.memAlloc);
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

            // Determine II: try --result file, then the mapping filename, then 1.
            int II = 0;
            if (!args.result.empty()) {
                // Parse "... <II>  status:success ..." from result.txt.
                std::ifstream rf(args.result);
                std::string line;
                while (std::getline(rf, line)) {
                    if (line.find("status:success") == std::string::npos) continue;
                    // Format: "<dfg> method:<m> <II>  status:..."
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
                II = ArchSchedule::parseIIFromFilename(args.mapping);
            }
            if (II <= 0) II = 1;

            CGRAMesh mesh(arch);
            ArchSchedule schedule = ArchSchedule::build(
                mapping, mesh, graph, II, args.maxHops);

            ArchSimulator sim(std::move(arch), std::move(schedule),
                              std::move(graph), std::move(mapping),
                              std::move(alloc), std::move(trace), options);
            result = sim.run();
        } else {
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
