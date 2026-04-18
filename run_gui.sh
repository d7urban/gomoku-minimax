#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${script_dir}/build-sfml"

cmake -S "${script_dir}" -B "${build_dir}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "${build_dir}"

exec "${build_dir}/gomoku_ui" "$@"
