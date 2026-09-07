#!/bin/bash

set -euo pipefail

support_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
runtime_dir=$(cd -- "$support_dir/../.." && pwd)
build_dir=${PLUGIN_SECURITY_SANITIZER_BUILD_DIR:-$runtime_dir/../../build/plugin-runtime-b6-sanitized}

cmake -S "$runtime_dir" -B "$build_dir" -G Ninja \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOMARCHY_PLUGIN_SANITIZERS=ON
cmake --build "$build_dir" --target \
  omarchy-plugin-support-test \
  omarchy-plugin-launcher-test

ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1} \
  ctest --test-dir "$build_dir" --output-on-failure \
    --tests-regex '^plugin-(test-support|launcher-contract|launcher-malicious-peer)$'
