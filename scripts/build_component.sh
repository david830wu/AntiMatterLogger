#!/usr/bin/env bash
set -Eeuo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

[ "$#" -eq 0 ] || { echo "Usage: $0" >&2; exit 2; }

prefix="${AM_PREFIX:-$repo_root/.local}"
build_type="${BUILD_TYPE:-Release}"
num_threads="${NUM_THREADS:-$(nproc)}"
build_dir="${AM_BUILD_DIR:-$repo_root/build}"
build_tests="${AM_BUILD_TESTS:-ON}"
cmake_prefix="$prefix${AM_CMAKE_PREFIX_PATH:+;$AM_CMAKE_PREFIX_PATH}${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}"

configure_args=(-S "$repo_root" -B "$build_dir")
[ "${AM_CLEAN_BUILD:-false}" != true ] || configure_args=(--fresh "${configure_args[@]}")
cmake "${configure_args[@]}" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$cmake_prefix" \
    -DAMC_BUILD_TESTS="$build_tests"
cmake --build "$build_dir" --parallel "$num_threads"
cmake --install "$build_dir"

printf 'AntiMatterLogger installed to %s\n' "$prefix"
