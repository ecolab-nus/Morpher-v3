#include "ArtifactSim/morpher/ArchJson.h"
#include "ArtifactSim/morpher/MemoryAccessProfile.h"

#include <sstream>
#include <string>

using namespace ArtifactSim;

int main() {
  Fabric fabric;
  fabric.xdim = 4;
  fabric.ydim = 1;
  fabric.pes.resize(4);
  fabric.pes[0].memoryEndpoints.push_back({"B0", "P0"});
  fabric.pes[1].memoryEndpoints.push_back({"B0", "P1"});
  // PE 2 is deliberately unbound.
  fabric.pes[3].memoryEndpoints.push_back({"B0", "P0"});
  fabric.pes[3].memoryEndpoints.push_back({"B1", "P0"});

  MemoryAccessProfiler profiler(fabric);
  profiler.recordIssue(0, 0, 4);
  profiler.recordIssue(0, 0, 4);
  profiler.recordIssue(1, 0, 8);
  profiler.recordIssue(0, 1, 4);
  profiler.recordIssue(1, 1, 4);
  profiler.recordIssue(2, 1, 4);
  profiler.recordIssue(3, 1, 4);

  const MemoryAccessStats stats = profiler.summarize();
  if (stats.issues != 7 || stats.bytes != 32) return 1;
  if (stats.unboundIssues != 1 || stats.ambiguousIssues != 1) return 2;
  if (stats.portConflictCycles != 1 || stats.excessTransactions != 1) return 3;
  if (stats.bankCapacityExcessTransactions != 1) return 4;
  if (stats.banks.size() != 2) return 5;
  const BankAccessStats &b0 = stats.banks[0];
  if (b0.bank != "B0" || b0.physicalPorts != 2 || b0.issues != 5 ||
      b0.bytes != 24 || b0.peakConcurrent != 3 ||
      b0.portConflictCycles != 1 || b0.excessTransactions != 1 ||
      b0.bankCapacityExcessTransactions != 1) return 6;

  std::ostringstream report;
  profiler.print(report);
  if (report.str().find("Issues::7") == std::string::npos ||
      report.str().find("Ambiguous::1") == std::string::npos) return 7;

  const MemoryAccessStats merged = mergeMemoryAccessStats({stats, stats});
  if (merged.issues != 14 || merged.bytes != 64 ||
      merged.portConflictCycles != 2 || merged.excessTransactions != 2 ||
      merged.banks[0].peakConcurrent != 3) return 12;

  const Fabric real = elaborateArchJson(TEST_ARCH_PATH);
  const auto &p0 = real.pe(0, 0).memoryEndpoints;
  const auto &p1 = real.pe(0, 1).memoryEndpoints;
  if (p0.size() != 1 || p1.size() != 1) return 8;
  if (p0[0].bank != "SPM_B0_WRAPPER_INS" ||
      p0[0].port != "MEMPORT_P0") return 9;
  if (p1[0].bank != "SPM_B0_WRAPPER_INS" ||
      p1[0].port != "MEMPORT_P1") return 10;
  if (!real.pe(1, 1).memoryEndpoints.empty()) return 11;
  return 0;
}
