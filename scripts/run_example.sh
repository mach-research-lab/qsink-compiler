#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${QSINK_BUILD_DIR:-"$source_dir/build"}
input=${1:-"$source_dir/examples/compact_layout_demo/compact_layout_demo.qasm"}
output=${2:-"$source_dir/output/demo"}
compiler="$build_dir/qsink"

if [[ ! -x "$compiler" ]]; then
    compiler="$build_dir/Release/qsink"
fi
if [[ ! -x "$compiler" ]]; then
    printf 'Run %s/build.sh to build the compiler first.\n' "$source_dir" >&2
    exit 1
fi

mkdir -p -- "$(dirname -- "$output")"
"$compiler" -q -i "$input" -L edpc --disttime 1 --nostagger -P wave \
    -o "$output"
