#include "ArtifactSim/functional/Simulator.h"
#include "ArtifactSim/morpher/ArchJson.h"
#include "ArtifactSim/morpher/BoundaryContract.h"
#include "ArtifactSim/morpher/CycleSimulator.h"
#include "ArtifactSim/morpher/MappingCompiler.h"

#include <fstream>
#include <iostream>
#include <regex>
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
    std::string arch;        // optional: arch JSON -> cycle-accurate mode
    std::string mappingJson; // optional: explicit mapping.json (else located
                             // next to --mapping)
    std::string result;      // optional: mapper result.txt (for II)
    std::string simdManifest; // optional: omitted-boundary simulation contract
    int maxIterations = 1024;
    std::vector<PhaseArg> extraPhases;
};

static void usage() {
    std::cerr
        << "Usage: artifact_simulator --dfg <xml> --mapping <mappingwithlatency.txt> "
        << "--mem-alloc <mem_alloc.txt> --trace <trace.txt> "
        << "[--max-iters <n>] [--addr-trace <addr_trace.txt>] "
        << "[--arch <arch.json>] [--mapping-json <mapping.json>] "
        << "[--result <result.txt>] [--simd-manifest <manifest.json>] "
        << "[--phase <dfg:mapping:mem_alloc>]...\n"
        << "Without --arch: functional golden checker (arch-unaware).\n"
        << "With --arch: cycle-accurate simulation on the elaborated fabric.\n";
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
        if      (opt == "--dfg")        args.dfg           = needValue(opt);
        else if (opt == "--mapping")    args.mapping        = needValue(opt);
        else if (opt == "--mem-alloc")  args.memAlloc       = needValue(opt);
        else if (opt == "--trace")      args.trace          = needValue(opt);
        else if (opt == "--addr-trace") args.addrTrace      = needValue(opt);
        else if (opt == "--arch")       args.arch           = needValue(opt);
        else if (opt == "--mapping-json") args.mappingJson  = needValue(opt);
        else if (opt == "--result")     args.result         = needValue(opt);
        else if (opt == "--simd-manifest") args.simdManifest = needValue(opt);
        else if (opt == "--phase")      args.extraPhases.push_back(parsePhaseSpec(needValue(opt)));
        else if (opt == "--max-iters")  args.maxIterations  = std::stoi(needValue(opt));
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

static int parseIIFromFilename(const std::string &path) {
    static const std::regex iiRe(R"(_II=(\d+)_)");
    std::smatch m;
    if (std::regex_search(path, m, iiRe)) {
        return std::stoi(m[1].str());
    }
    return 0;
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
        II = parseIIFromFilename(mappingPath);
    }
    return II > 0 ? II : 1;
}

int main(int argc, char **argv) {
    try {
        Args args = parseArgs(argc, argv);

        std::vector<PhaseArg> phases;
        phases.push_back(PhaseArg{args.dfg, args.mapping, args.memAlloc});
        phases.insert(phases.end(), args.extraPhases.begin(), args.extraPhases.end());

        MemAlloc alloc   = parseMemAlloc(phases.front().memAlloc);
        TraceImage trace = parseTrace(args.trace, alloc);
        if (!args.addrTrace.empty()) {
            parseAddressTrace(args.addrTrace, trace);
        }
        const BoundaryContract boundary =
            parseBoundaryContract(args.simdManifest);
        if (!boundary.empty()) {
            std::cout
                << "Simulation scope: CGRA schedules the steady-state loop DFG only.\n"
                << "Omitted boundary work: vector-accumulator initialization in "
                   "the loop preheader; VRED, scalar combine, and result store "
                   "at loop exit.\n"
                << "Boundary model: recurrence lanes are seeded from traced "
                   "preheader values (checked against the manifest identity); "
                   "final vector lanes and the scalar exit are validated against "
                   "the source trace.\n";
        }

        SimResult result;

        if (!args.arch.empty()) {
            // Cycle-accurate mode: elaborate the fabric from the arch JSON and
            // compile the mapper's routing record into configuration memory.
            const Fabric fabric = elaborateArchJson(args.arch);
            std::map<std::pair<int, int>, Value> macState;

            for (size_t idx = 0; idx < phases.size(); ++idx) {
                const PhaseArg &phase = phases[idx];
                DFGGraph graph = DFGGraph::parse(phase.dfg);
                MemAlloc phaseAlloc = parseMemAlloc(phase.memAlloc);
                const int II = resolveII(
                    phase.mapping, idx == 0 ? args.result : std::string{});

                std::string mappingJson =
                    (idx == 0 && !args.mappingJson.empty())
                        ? args.mappingJson
                        : findMappingJson(phase.mapping);
                if (mappingJson.empty()) {
                    throw std::runtime_error(
                        "no mapping.json found next to " + phase.mapping +
                        " (required for cycle-accurate mode)");
                }

                ConfigMem config = compileMapping(fabric, graph, mappingJson, II);
                BoundaryContract phaseBoundary =
                    idx == 0 ? boundary : BoundaryContract{};
                CycleSimulator sim(fabric, config, graph,
                                   std::move(phaseAlloc), trace,
                                   args.maxIterations, std::move(phaseBoundary));
                SimResult phaseResult =
                    sim.run(macState, idx + 1 == phases.size());
                result.matches += phaseResult.matches;
                result.mismatches += phaseResult.mismatches;
                trace = sim.traceImage();
            }
        } else {
            if (phases.size() != 1) {
                throw std::runtime_error(
                    "--phase requires --arch so PE-local state is defined");
            }
            DFGGraph graph = DFGGraph::parse(phases.front().dfg);
            FunctionalSimulator sim(std::move(graph), std::move(alloc),
                                    std::move(trace), args.maxIterations,
                                    boundary);
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
