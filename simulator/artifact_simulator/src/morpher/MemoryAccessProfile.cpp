#include "ArtifactSim/morpher/MemoryAccessProfile.h"

#include <algorithm>
#include <ostream>

namespace ArtifactSim {

MemoryAccessStats mergeMemoryAccessStats(
    const std::vector<MemoryAccessStats> &profiles) {
  MemoryAccessStats result;
  std::map<std::string, BankAccessStats> banks;
  for (const MemoryAccessStats &profile : profiles) {
    result.issues += profile.issues;
    result.bytes += profile.bytes;
    result.unboundIssues += profile.unboundIssues;
    result.ambiguousIssues += profile.ambiguousIssues;
    result.portConflictCycles += profile.portConflictCycles;
    result.excessTransactions += profile.excessTransactions;
    result.bankCapacityExcessTransactions +=
        profile.bankCapacityExcessTransactions;
    for (const BankAccessStats &bank : profile.banks) {
      BankAccessStats &merged = banks[bank.bank];
      merged.bank = bank.bank;
      merged.issues += bank.issues;
      merged.bytes += bank.bytes;
      merged.portConflictCycles += bank.portConflictCycles;
      merged.excessTransactions += bank.excessTransactions;
      merged.bankCapacityExcessTransactions +=
          bank.bankCapacityExcessTransactions;
      merged.physicalPorts = std::max(merged.physicalPorts,
                                      bank.physicalPorts);
      merged.peakConcurrent = std::max(merged.peakConcurrent,
                                       bank.peakConcurrent);
    }
  }
  for (const auto &[name, bank] : banks) {
    result.banks.push_back(bank);
  }
  return result;
}

MemoryAccessProfiler::MemoryAccessProfiler(const Fabric &fabric)
    : fabric_(fabric) {
  for (const FabricPE &pe : fabric_.pes) {
    for (const MemoryEndpoint &endpoint : pe.memoryEndpoints) {
      physicalPorts_[endpoint.bank].insert(endpoint.port);
    }
  }
}

void MemoryAccessProfiler::recordIssue(int pe, int cycle, uint64_t bytes) {
  ++issues_;
  bytes_ += bytes;
  if (pe < 0 || pe >= static_cast<int>(fabric_.pes.size()) ||
      fabric_.pes[static_cast<size_t>(pe)].memoryEndpoints.empty()) {
    ++unboundIssues_;
    return;
  }
  const auto &endpoints =
      fabric_.pes[static_cast<size_t>(pe)].memoryEndpoints;
  if (endpoints.size() != 1) {
    ++ambiguousIssues_;
    return;
  }

  const MemoryEndpoint &endpoint = endpoints.front();
  ++bankIssues_[endpoint.bank];
  bankBytes_[endpoint.bank] += bytes;
  ++portIssues_[{cycle, endpoint.bank, endpoint.port}];
  ++bankCycleIssues_[{cycle, endpoint.bank}];
}

MemoryAccessStats MemoryAccessProfiler::summarize() const {
  MemoryAccessStats result;
  result.issues = issues_;
  result.bytes = bytes_;
  result.unboundIssues = unboundIssues_;
  result.ambiguousIssues = ambiguousIssues_;

  std::map<std::string, BankAccessStats> banks;
  for (const auto &[bank, ports] : physicalPorts_) {
    BankAccessStats &stats = banks[bank];
    stats.bank = bank;
    stats.physicalPorts = static_cast<int>(ports.size());
  }
  for (const auto &[bank, count] : bankIssues_) {
    BankAccessStats &stats = banks[bank];
    stats.bank = bank;
    stats.issues = count;
    stats.bytes = bankBytes_.at(bank);
  }

  std::map<std::string, std::set<int>> conflictCycles;
  for (const auto &[key, count] : portIssues_) {
    if (count <= 1) {
      continue;
    }
    const int cycle = std::get<0>(key);
    const std::string &bank = std::get<1>(key);
    const uint64_t excess = count - 1;
    conflictCycles[bank].insert(cycle);
    banks[bank].excessTransactions += excess;
    result.excessTransactions += excess;
  }
  for (const auto &[bank, cycles] : conflictCycles) {
    banks[bank].portConflictCycles = cycles.size();
    result.portConflictCycles += cycles.size();
  }

  for (const auto &[key, count] : bankCycleIssues_) {
    const std::string &bank = key.second;
    BankAccessStats &stats = banks[bank];
    stats.peakConcurrent = std::max(stats.peakConcurrent,
                                    static_cast<int>(count));
    const uint64_t capacity = static_cast<uint64_t>(stats.physicalPorts);
    if (capacity > 0 && count > capacity) {
      const uint64_t excess = count - capacity;
      stats.bankCapacityExcessTransactions += excess;
      result.bankCapacityExcessTransactions += excess;
    }
  }

  for (const auto &[bank, stats] : banks) {
    result.banks.push_back(stats);
  }
  return result;
}

void MemoryAccessProfiler::print(std::ostream &out) const {
  const MemoryAccessStats stats = summarize();
  out << "Memory Access Profile: Issues::" << stats.issues
      << ", Bytes::" << stats.bytes
      << ", Banks::" << stats.banks.size()
      << ", PortConflictCycles::" << stats.portConflictCycles
      << ", ExcessTransactions::" << stats.excessTransactions
      << ", BankCapacityExcess::"
      << stats.bankCapacityExcessTransactions
      << ", Unbound::" << stats.unboundIssues
      << ", Ambiguous::" << stats.ambiguousIssues << "\n";
  for (const BankAccessStats &bank : stats.banks) {
    out << "Memory Bank " << bank.bank
        << ": Ports::" << bank.physicalPorts
        << ", Issues::" << bank.issues
        << ", Bytes::" << bank.bytes
        << ", PeakConcurrent::" << bank.peakConcurrent
        << ", PortConflictCycles::" << bank.portConflictCycles
        << ", ExcessTransactions::" << bank.excessTransactions
        << ", BankCapacityExcess::"
        << bank.bankCapacityExcessTransactions << "\n";
  }
}

} // namespace ArtifactSim
