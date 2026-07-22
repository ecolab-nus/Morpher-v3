#pragma once

#include <string>

namespace CGRAXMLCompile {

// GEPOffset is absent on outer-loop memory nodes even after the mapper has
// materialized their absolute address in `constant`. Recover only for those
// nodes: applying constant-base to a dynamic regular LOAD/STORE would invent
// an offset that the DFG does not actually know.
inline int layoutObservationGepOffset(const std::string &op, int gepOffset,
                                      int materializedAddress,
                                      const int *baseAddress) {
  if (gepOffset != -1 || baseAddress == nullptr) {
    return gepOffset;
  }
  const bool outerMemory = op.find("OLOAD") != std::string::npos ||
                           op.find("OSTORE") != std::string::npos;
  return outerMemory ? materializedAddress - *baseAddress : -1;
}

} // namespace CGRAXMLCompile
