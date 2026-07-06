#pragma once

#include "ArtifactSim/core/ConfigMem.h"
#include "ArtifactSim/core/Fabric.h"
#include "ArtifactSim/morpher/DFG.h"

#include <string>

namespace ArtifactSim {

// Compiles the mapper's routing record (mapping.json) into the fabric's
// configuration memory.
//
// The mapping.json carries, per PE:
//   - CONNECTIONS: {slot: {src: [dst...]}} -- the mux program actually used.
//     "<REG>_RO" refs are the register's write side, "<REG>_RI" the read
//     side (one-cycle latch between them, per the mapper's MRRG wiring).
//   - MAPPED_NODES: {slot: {port: node}} -- which node's value rides each
//     port (used for diagnostics and operand cross-checks).
//   - FU OP_INFO: {slot: {id}} -- issue slot per node; the result lands on
//     the FU result register `latency` cycles later and persists.
// Slot keys are mod-II.
//
// Every route select is validated against fabric edges, so a mapping that
// crosses a connection absent from (e.g. faulted out of) the arch JSON fails
// here with a precise diagnostic.
//
// Issue start cycles are the smallest cycles congruent to the issue slot that
// satisfy all same-iteration data dependencies (see startCycle relaxation in
// the implementation); this reconstructs the pipeline fill order.
ConfigMem compileMapping(const Fabric &fabric, const DFGGraph &graph,
                         const std::string &mappingJsonPath, int II);

// Locates the mapping.json that belongs to a mappingwithlatency.txt path
// (same directory scan the old Mapping parser used). Returns "" if absent.
std::string findMappingJson(const std::string &mappingPath);

} // namespace ArtifactSim
