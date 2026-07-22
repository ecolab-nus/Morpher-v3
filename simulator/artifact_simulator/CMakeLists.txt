cmake_minimum_required(VERSION 3.10)
project(morpher_artifact_simulator LANGUAGES CXX)

add_executable(artifact_simulator
  src/main.cpp
  src/core/Value.cpp
  src/core/Ops.cpp
  src/core/Fabric.cpp
  src/core/Engine.cpp
  src/morpher/ArchJson.cpp
  src/morpher/BoundaryContract.cpp
  src/morpher/MappingCompiler.cpp
  src/morpher/NodeSemantics.cpp
  src/morpher/CycleSimulator.cpp
  src/morpher/DFG.cpp
  src/morpher/Memory.cpp
  src/morpher/MemoryAccessProfile.cpp
  src/morpher/Trace.cpp
  src/functional/Simulator.cpp
  ../../Morpher_CGRA_Mapper/src/util/tinyxml2.cpp
)

target_include_directories(artifact_simulator PRIVATE
  include
  ../../Morpher_CGRA_Mapper/include
)

target_compile_features(artifact_simulator PRIVATE cxx_std_17)

include(CTest)
if(BUILD_TESTING)
  add_executable(launch_reset_test
    tests/LaunchResetTest.cpp
    src/core/Value.cpp
    src/core/Ops.cpp
    src/morpher/BoundaryContract.cpp
  )
  target_include_directories(launch_reset_test PRIVATE
    include
    ../../Morpher_CGRA_Mapper/include
  )
  target_compile_features(launch_reset_test PRIVATE cxx_std_17)
  add_test(NAME launch_reset_test COMMAND launch_reset_test)

  add_executable(memory_access_profile_test
    tests/MemoryAccessProfileTest.cpp
    src/core/Fabric.cpp
    src/morpher/ArchJson.cpp
    src/morpher/MemoryAccessProfile.cpp
  )
  target_include_directories(memory_access_profile_test PRIVATE
    include
    ../../Morpher_CGRA_Mapper/include
  )
  target_compile_features(memory_access_profile_test PRIVATE cxx_std_17)
  target_compile_definitions(memory_access_profile_test PRIVATE
    TEST_ARCH_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../Morpher_CGRA_Mapper/json_arch/edge_mem_4x4/scalar_3side.json"
  )
  add_test(NAME memory_access_profile_test COMMAND memory_access_profile_test)
endif()
