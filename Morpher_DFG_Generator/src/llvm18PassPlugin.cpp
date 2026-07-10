#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

FunctionPassManager createMorpherDFGGenPipeline();
FunctionPassManager createMorpherLoopUnrollPipeline();
FunctionPassManager createMorpherLoopInterchangePipeline();
FunctionPassManager createMorpherSystolicCandidatePipeline();

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "morpher-llvm18-plugin", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "skeleton") {
                MPM.addPass(
                    createModuleToFunctionPassAdaptor(createMorpherDFGGenPipeline()));
                return true;
              }
              if (Name == "morpher-loop-unroll") {
                MPM.addPass(createModuleToFunctionPassAdaptor(
                    createMorpherLoopUnrollPipeline()));
                return true;
              }
              if (Name == "morpher-loop-interchange") {
                MPM.addPass(createModuleToFunctionPassAdaptor(
                    createMorpherLoopInterchangePipeline()));
                return true;
              }
              if (Name == "morpher-systolic-candidate") {
                MPM.addPass(createModuleToFunctionPassAdaptor(
                    createMorpherSystolicCandidatePipeline()));
                return true;
              }
              return false;
            });
      }};
}
