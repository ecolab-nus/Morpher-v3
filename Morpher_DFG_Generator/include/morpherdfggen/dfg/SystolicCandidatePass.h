#ifndef MORPHERDFGGEN_DFG_SYSTOLICCANDIDATEPASS_H
#define MORPHERDFGGEN_DFG_SYSTOLICCANDIDATEPASS_H

#include "llvm/IR/PassManager.h"

llvm::FunctionPassManager createMorpherSystolicCandidatePipeline();
llvm::FunctionPassManager createMorpherSystolicUnrollJamPipeline();

#endif
