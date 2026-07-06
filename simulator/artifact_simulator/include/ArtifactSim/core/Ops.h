#pragma once

#include "ArtifactSim/core/Value.h"

#include <string>

namespace ArtifactSim {

// Single source of truth for operation semantics.
// A leading 'V' marks the SIMD variant of an op; semantics are identical
// per-lane, so it is stripped before lookup (VADD -> ADD).
//
// Unknown opcodes throw: silently returning an operand (the old behaviour)
// turns a lowering bug into a wrong simulation result.
std::string baseOpName(const std::string &op);

bool isKnownAluOp(const std::string &op);

Value unaryOp(const Value &a, const std::string &op);
Value binaryOp(const Value &a, const Value &b, const std::string &op);

// Horizontal reduction across SIMD lanes (VRED). Supported: ADD/AND/OR/XOR.
uint32_t reduceLanes(const std::vector<uint32_t> &lanes, const std::string &op);

} // namespace ArtifactSim
