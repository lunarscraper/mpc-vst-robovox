#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan) of robovox_vst.cpp, then a benchmark (-O2): run after build.sh
# (needs build/params.h, build/popup.h). Prints PASSED/FAILED; exit code follows. The benchmark is
# for the CI machine's x86 core: on the Force's Cortex-A17 expect several times more -
# /tmp/robovox_vst.log on the device has the real load.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/b -w /b gcc:12 bash -euc 'set -o pipefail
  SAN="-fsanitize=address,undefined"
  mkdir -p build/x86 build/x86o
  g++ -O0 -g $SAN -std=c++17 -fPIC -Ibuild -shared -o build/x86/robovox-x86.so robovox_vst.cpp -lpthread -ldl
  g++ -O0 -g $SAN -std=c++17 -o build/x86/host_test host_test.cpp -ldl
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/robovox-x86.so

  g++ -O2 -std=c++17 -fPIC -fvisibility=hidden -Ibuild -shared -o build/x86o/robovox-x86.so robovox_vst.cpp -lpthread -ldl
  g++ -O2 -std=c++17 -o build/x86o/host_test host_test.cpp -ldl
  ./build/x86o/host_test ./build/x86o/robovox-x86.so bench
'
