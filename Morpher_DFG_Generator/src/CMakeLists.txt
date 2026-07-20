llvm_add_library(dfggenPass MODULE
    OUTPUT_NAME "libdfggenPass"
    PLUGIN_TOOL opt
    passes/dfggen/DfgGenInternal.h
    passes/dfggen/IrPrep.cpp
    passes/dfggen/MappingUnits.cpp
    passes/dfggen/SpmBanks.cpp
    passes/dfggen/DfgGenDriver.cpp
    passes/unroll/UnrollInternal.h
    passes/unroll/LoopUnroll.cpp
    passes/unroll/MarkerPasses.cpp
    passes/unroll/LoopInterchange.cpp
    passes/unroll/SimdReductionExpansion.cpp
    ../include/morpherdfggen/arch/CGRANode.h
    arch/CGRANode.cpp
    ../include/morpherdfggen/arch/CGRA.h
    arch/CGRA.cpp
    ../include/morpherdfggen/common/edge.h
    common/edge.cpp
    ../include/morpherdfggen/common/dfgnode.h
    common/dfgnode.cpp
    ../include/morpherdfggen/common/dfg.h
    common/dfg.cpp
    ../include/morpherdfggen/common/astar.h
    common/astar.cpp
    llvm18PassPlugin.cpp
    ../include/morpherdfggen/util/tinyxml2.h
    util/tinyxml2.cpp
    common/dfgmacro.cpp
    ../include/morpherdfggen/common/dfgmacro.h
    ../include/morpherdfggen/dfg/DFGPartPred.h
    dfg/DFGPartPred.cpp
    ../include/morpherdfggen/dfg/SystolicCandidatePass.h
    passes/systolic/SystolicInternal.h
    passes/systolic/NestAnalysis.cpp
    passes/systolic/DetectPass.cpp
    passes/systolic/MarkerPasses.cpp
    passes/systolic/CanonicalizeOutput.cpp
    passes/systolic/ConfigureUnrollJam.cpp
    passes/systolic/RestoreStreams.cpp
    passes/systolic/PollySchedule.cpp
)

find_path(POLLY_INCLUDE_DIR polly/ScopPass.h
    HINTS ${LLVM_INCLUDE_DIRS}
    DOC "LLVM-version-matched Polly development headers")
if(NOT POLLY_INCLUDE_DIR)
    message(FATAL_ERROR
        "Systolic exact scheduling requires the LLVM-version-matched Polly "
        "development headers (Ubuntu package: libpolly-18-dev).")
endif()

target_include_directories(dfggenPass PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${POLLY_INCLUDE_DIR}
    ${POLLY_INCLUDE_DIR}/polly
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_options(dfggenPass PRIVATE -fno-rtti -fexceptions -ggdb)

if(APPLE)
    target_link_options(dfggenPass PRIVATE "-undefined" "dynamic_lookup")
endif()
