#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${1:-"$ROOT/build"}
PREFIX="$BUILD/install"
cmake --install "$BUILD" --prefix "$PREFIX"
cmake -S "$ROOT/tests/consumer" -B "$BUILD/consumer" -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build "$BUILD/consumer" --parallel 2
ctest --test-dir "$BUILD/consumer" --output-on-failure
