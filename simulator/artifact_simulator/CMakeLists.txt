cmake_minimum_required(VERSION 3.10)
project(morpher_artifact_simulator LANGUAGES CXX)

add_executable(artifact_simulator
  src/main.cpp
  src/core/Value.cpp
  src/core/Ops.cpp
  src/core/Fabric.cpp
  src/core/Engine.cpp
  src/morpher/ArchJson.cpp
  src/morpher/MappingCompiler.cpp
  src/morpher/NodeSemantics.cpp
  src/morpher/CycleSimulator.cpp
  src/morpher/DFG.cpp
  src/morpher/Memory.cpp
  src/morpher/Trace.cpp
  src/functional/Simulator.cpp
  ../../Morpher_CGRA_Mapper/src/util/tinyxml2.cpp
)

target_include_directories(artifact_simulator PRIVATE
  include
  ../../Morpher_CGRA_Mapper/include
)

target_compile_features(artifact_simulator PRIVATE cxx_std_17)
