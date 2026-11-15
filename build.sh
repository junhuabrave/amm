#!/bin/sh
# Fallback build without CMake: needs only a C++20 compiler.
set -e
CXX=${CXX:-g++}
mkdir -p build
for t in apps/amm_sim apps/amm_bench tests/test_all; do
  $CXX -std=c++20 -O2 -Wall -Wextra -Iinclude $t.cpp -o build/$(basename $t)
done
mv build/test_all build/amm_tests
echo "built: build/amm_sim build/amm_bench build/amm_tests"
