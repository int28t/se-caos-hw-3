#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-none}
case "$mode" in
  none) build=build; sanitizers=OFF;;
  asan) build=build-asan; sanitizers=ON;;
  *) echo 'Использование: tests/run.sh none|asan' >&2; exit 2;;
esac
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DENABLE_SANITIZERS="$sanitizers"
cmake --build "$build" --parallel
ctest --test-dir "$build" --output-on-failure
