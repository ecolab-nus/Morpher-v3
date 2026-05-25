llvm_add_library(dfggenPass MODULE
    OUTPUT_NAME "libdfggenPass"
    PLUGIN_TOOL opt
    dfggenPass.cpp
    morpherLoopUnrollPass.cpp
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
    dfg/SystolicCandidatePass.cpp
)

target_include_directories(dfggenPass PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_options(dfggenPass PRIVATE -fno-rtti -fexceptions -ggdb)

if(APPLE)
    target_link_options(dfggenPass PRIVATE "-undefined" "dynamic_lookup")
endif()
