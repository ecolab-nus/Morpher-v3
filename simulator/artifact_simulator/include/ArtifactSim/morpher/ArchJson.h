#pragma once

#include "ArtifactSim/core/Fabric.h"

#include <string>

namespace ArtifactSim {

// Elaborates a Morpher arch JSON into a Fabric.
//
// Handled schema (what the mapper's json_arch files actually use):
//   ARCH.CGRA.SUBMODS[PATTERN=="GRID"] with DIMS.X/Y and per-tile MODS
//   overrides; PE-like modules with INPUTS/OUTPUTS/INTERNALS/REGS/SUBMODS/
//   CONNECTIONS; FU-like submodules with INPUTS/OUTPUTS/OPS (op -> latency).
//   ARCH.CGRA.CONNECTIONS entries of the form
//   PE_MEM_Xx_Yy.MEMPORT -> SPM_bank.MEMPORT_Pn are retained as physical
//   memory endpoints for access profiling.
//   Faulty archs (PE_Fault_*/FU_Fault_*) need no special handling: they are
//   ordinary module types whose CONNECTIONS simply lack some edges.
//
// Grid wiring replicates Morpher_CGRA_Mapper/src/arch/CGRA.cpp: bounded grid
// (no torus), NORTH_O(x,y) -> SOUTH_I(x,y-1) etc. Inter-PE links are
// combinational (HyCUBE multi-hop); only REGS and FU result ports hold state.
//
// Assumptions asserted at load time (fail loudly rather than mismodel):
//   - exactly one FU-like submodule per PE (named e.g. "FU0"),
//   - FU operand ports named <dp>_I1/_I2/_P and result <dp>_T.
Fabric elaborateArchJson(const std::string &jsonPath);

} // namespace ArtifactSim
