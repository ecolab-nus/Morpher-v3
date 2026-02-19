#!/bin/bash
set -euo pipefail

cd Morpher_DFG_Generator
mkdir -p build
cd build
cmake ..
make all -j "$(nproc)"
cd ../../Morpher_CGRA_Mapper
mkdir -p build
cd build
cmake ..
make all -j "$(nproc)"
cd ../../hycube_simulator
cd src
mkdir -p build
cd build
cmake ..
make all -j "$(nproc)"
echo "build success!!!"
