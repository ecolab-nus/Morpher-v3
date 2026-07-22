#pragma once

#include "ArtifactSim/core/Fabric.h"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ArtifactSim {

struct BankAccessStats {
  std::string bank;
  uint64_t issues = 0;
  uint64_t bytes = 0;
  uint64_t portConflictCycles = 0;
  uint64_t excessTransactions = 0;
  uint64_t bankCapacityExcessTransactions = 0;
  int physicalPorts = 0;
  int peakConcurrent = 0;
};

struct MemoryAccessStats {
  uint64_t issues = 0;
  uint64_t bytes = 0;
  uint64_t unboundIssues = 0;
  uint64_t ambiguousIssues = 0;
  uint64_t portConflictCycles = 0;
  uint64_t excessTransactions = 0;
  uint64_t bankCapacityExcessTransactions = 0;
  std::vector<BankAccessStats> banks;
};

MemoryAccessStats mergeMemoryAccessStats(
    const std::vector<MemoryAccessStats> &profiles);

// Observational profiler for memory operations issued by the cycle engine.
// It deliberately does not delay operations: the current engine has no
// ready/valid or backpressure semantics. One rigid PE->bank/port endpoint can
// be attributed exactly; zero or multiple endpoints are reported rather than
// guessed. Each SIMD issue is one port transaction and contributes its full
// lane-scaled byte width.
class MemoryAccessProfiler {
public:
  explicit MemoryAccessProfiler(const Fabric &fabric);

  void recordIssue(int pe, int cycle, uint64_t bytes);
  MemoryAccessStats summarize() const;
  void print(std::ostream &out) const;

private:
  const Fabric &fabric_;
  uint64_t issues_ = 0;
  uint64_t bytes_ = 0;
  uint64_t unboundIssues_ = 0;
  uint64_t ambiguousIssues_ = 0;
  std::map<std::string, std::set<std::string>> physicalPorts_;
  std::map<std::string, uint64_t> bankIssues_;
  std::map<std::string, uint64_t> bankBytes_;
  std::map<std::tuple<int, std::string, std::string>, uint64_t> portIssues_;
  std::map<std::pair<int, std::string>, uint64_t> bankCycleIssues_;
};

} // namespace ArtifactSim
