#!/bin/sh
# Fallback build without CMake: needs only a C++20 compiler.
set -e
CXX=${CXX:-g++}
mkdir -p build
for t in apps/amm_sim apps/amm_bench apps/amm_run apps/amm_calibrate apps/amm_record_sim tests/test_all; do
  $CXX -std=c++20 -O2 -Wall -Wextra -Iinclude -DAMM_SOURCE_DIR="\"$(pwd)\"" $t.cpp -o build/$(basename $t)
done
mv build/test_all build/amm_tests
echo "built in build/: amm_sim amm_bench amm_run amm_calibrate amm_record_sim amm_tests"
