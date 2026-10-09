#!/usr/bin/env bash
# Build and test using system dependencies, or the prepared cloud prefix.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cloud_tools=/workspace/.provider-tools
cmake_command=cmake
ctest_command=ctest
configure_args=()
if [[ -x "$cloud_tools/python/cmake/data/bin/cmake" && -d "$cloud_tools/sysroot/usr" ]]; then
  cmake_command="$cloud_tools/python/cmake/data/bin/cmake"
  ctest_command="$cloud_tools/python/cmake/data/bin/ctest"
  export PKG_CONFIG_PATH="$cloud_tools/sysroot/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  configure_args+=("-DCMAKE_PREFIX_PATH=$cloud_tools/sysroot/usr" "-G" "Ninja" "-DCMAKE_MAKE_PROGRAM=$cloud_tools/python/bin/ninja")
fi
"$cmake_command" -S "$repo_dir" -B "$repo_dir/build" "${configure_args[@]}"
"$cmake_command" --build "$repo_dir/build" -j "${BUILD_JOBS:-2}"
"$ctest_command" --test-dir "$repo_dir/build" --output-on-failure
