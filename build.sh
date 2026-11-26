#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build_dir=${QSINK_BUILD_DIR:-"$source_dir/build"}
build_jobs=${QSINK_BUILD_JOBS:-4}

cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --config Release --target qsink --parallel "$build_jobs"

if [[ -x "$build_dir/qsink" ]]; then
    printf 'Compiler executable is at %s\n' "$build_dir/qsink"
else
    printf 'Compiler executable is at %s\n' "$build_dir/Release/qsink"
fi
