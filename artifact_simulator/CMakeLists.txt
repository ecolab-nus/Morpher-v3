cmake_minimum_required(VERSION 3.10)
project(morpher_artifact_simulator LANGUAGES CXX)

add_executable(artifact_simulator
  src/main.cpp
  src/ArtifactSimulator.cpp
  src/DFG.cpp
  src/Mapping.cpp
  src/Memory.cpp
  src/Mesh.cpp
  src/Trace.cpp
  src/Value.cpp
  ../Morpher_CGRA_Mapper/src/util/tinyxml2.cpp
)

target_include_directories(artifact_simulator PRIVATE
  include
  ../Morpher_CGRA_Mapper/include
)

target_compile_features(artifact_simulator PRIVATE cxx_std_17)
