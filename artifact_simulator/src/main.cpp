#include "ArtifactSim/ArtifactSimulator.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace ArtifactSim;

struct Args {
  std::string dfg;
  std::string mapping;
  std::string memAlloc;
  std::string trace;
  std::string addrTrace;
  int xdim = 4;
  int ydim = 4;
  int maxIterations = 1024;
};

static void usage() {
  std::cerr
      << "Usage: artifact_simulator --dfg <xml> --mapping <mappingwithlatency.txt> "
      << "--mem-alloc <mem_alloc.txt> --trace <trace.txt> [--x <n>] [--y <n>] "
      << "[--max-iters <n>] [--addr-trace <addr_trace.txt>]\n";
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
    if (opt == "--dfg") args.dfg = needValue(opt);
    else if (opt == "--mapping") args.mapping = needValue(opt);
    else if (opt == "--mem-alloc") args.memAlloc = needValue(opt);
    else if (opt == "--trace") args.trace = needValue(opt);
    else if (opt == "--addr-trace") args.addrTrace = needValue(opt);
    else if (opt == "--x") args.xdim = std::stoi(needValue(opt));
    else if (opt == "--y") args.ydim = std::stoi(needValue(opt));
    else if (opt == "--max-iters") args.maxIterations = std::stoi(needValue(opt));
    else if (opt == "--help") {
      usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option: " + opt);
    }
  }
  if (args.dfg.empty() || args.mapping.empty() || args.memAlloc.empty() || args.trace.empty()) {
    usage();
    throw std::runtime_error("missing required arguments");
  }
  return args;
}

int main(int argc, char **argv) {
  try {
    Args args = parseArgs(argc, argv);
    DFGGraph graph = DFGGraph::parse(args.dfg);
    Mapping mapping = Mapping::parse(args.mapping);
    MemAlloc alloc = parseMemAlloc(args.memAlloc);
    TraceImage trace = parseTrace(args.trace, alloc);
    if (!args.addrTrace.empty()) {
      parseAddressTrace(args.addrTrace, trace);
    }
    SimOptions options;
    options.xdim = args.xdim;
    options.ydim = args.ydim;
    options.maxIterations = args.maxIterations;

    Simulator sim(std::move(graph), std::move(mapping), std::move(alloc), std::move(trace), options);
    SimResult result = sim.run();
    std::cout << "Simulation Result: Matches::" << result.matches
              << ", Mismatches::" << result.mismatches << "\n";
    return result.mismatches == 0 ? 0 : 2;
  } catch (const std::exception &exc) {
    std::cerr << "Error: " << exc.what() << "\n";
    return 1;
  }
}
