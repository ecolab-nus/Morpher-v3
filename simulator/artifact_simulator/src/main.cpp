#include "ArtifactSim/functional/Simulator.h"
#include "ArtifactSim/morpher/ArchJson.h"
#include "ArtifactSim/morpher/BoundaryContract.h"
#include "ArtifactSim/morpher/CycleSimulator.h"
#include "ArtifactSim/morpher/MappingCompiler.h"
#include "ArtifactSim/morpher/MemoryAccessProfile.h"
#include "morpher/arch/json.hpp"

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ArtifactSim;
using json = nlohmann::json;

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
    std::string boundaryManifest; // optional: scalar/vector state boundary contract
    std::string memoryProfileJson; // optional: observational cycle-mode counters
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
        << "[--boundary-manifest <manifest.json>] "
        << "[--memory-profile-json <profile.json>] "
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
        else if (opt == "--boundary-manifest") args.boundaryManifest = needValue(opt);
        else if (opt == "--memory-profile-json") args.memoryProfileJson = needValue(opt);
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

static json memoryStatsJson(const MemoryAccessStats &stats) {
    json out;
    out["issues"] = stats.issues;
    out["bytes"] = stats.bytes;
    out["unbound_issues"] = stats.unboundIssues;
    out["ambiguous_issues"] = stats.ambiguousIssues;
    out["port_conflict_cycles"] = stats.portConflictCycles;
    out["excess_transactions"] = stats.excessTransactions;
    out["bank_capacity_excess_transactions"] =
        stats.bankCapacityExcessTransactions;
    out["banks"] = json::array();
    for (const BankAccessStats &bank : stats.banks) {
        out["banks"].push_back({
            {"bank", bank.bank},
            {"physical_ports", bank.physicalPorts},
            {"issues", bank.issues},
            {"bytes", bank.bytes},
            {"peak_concurrent", bank.peakConcurrent},
            {"port_conflict_cycles", bank.portConflictCycles},
            {"excess_transactions", bank.excessTransactions},
            {"bank_capacity_excess_transactions",
             bank.bankCapacityExcessTransactions},
        });
    }
    return out;
}

static void writeMemoryProfile(
    const std::string &path,
    const std::vector<MemoryAccessStats> &phaseProfiles) {
    json doc;
    doc["schema_version"] = 1;
    doc["kind"] = "morpher_memory_access_profile";
    doc["timing_effect"] = "observational_only";
    doc["phases"] = json::array();
    for (size_t i = 0; i < phaseProfiles.size(); ++i) {
        doc["phases"].push_back({
            {"phase", i},
            {"stats", memoryStatsJson(phaseProfiles[i])},
        });
    }
    doc["aggregate"] =
        memoryStatsJson(mergeMemoryAccessStats(phaseProfiles));

    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("cannot write memory profile JSON: " + path);
    }
    out << doc.dump(2) << "\n";
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
        if (!args.simdManifest.empty() && !args.boundaryManifest.empty()) {
            throw std::runtime_error(
                "--simd-manifest and --boundary-manifest cannot both describe omitted boundaries");
        }
        const BoundaryContract boundary = parseBoundaryContract(
            !args.boundaryManifest.empty() ? args.boundaryManifest
                                           : args.simdManifest);
        if (!boundary.empty()) {
            std::cout
                << "Simulation scope: CGRA schedules the steady-state loop DFG only.\n"
                << "Omitted boundary work: loop-launch state initialization and "
                   "loop-exit live-out drains.\n"
                << "Boundary model: mapped recurrence state is initialized from "
                   "the validated manifest and final state is checked against "
                   "retained source-trace values.\n";
        }

        SimResult result;
        std::vector<MemoryAccessStats> phaseMemoryProfiles;

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
                phaseMemoryProfiles.push_back(sim.memoryAccessStats());
                trace = sim.traceImage();
            }
        } else {
            if (!args.memoryProfileJson.empty()) {
                throw std::runtime_error(
                    "--memory-profile-json requires cycle-accurate mode (--arch)");
            }
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

        if (!args.memoryProfileJson.empty()) {
            writeMemoryProfile(args.memoryProfileJson, phaseMemoryProfiles);
        }

        std::cout << "Simulation Result: Matches::" << result.matches
                  << ", Mismatches::" << result.mismatches << "\n";
        return result.mismatches == 0 ? 0 : 2;

    } catch (const std::exception &exc) {
        std::cerr << "Error: " << exc.what() << "\n";
        return 1;
    }
}
