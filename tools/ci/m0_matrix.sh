#!/bin/sh
# Build and test the M0 matrix:
#   LJ_MT off/on x JIT on/off.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

run_combo() {
  name=$1
  xcflags=$2
  stock_tags=$3

  echo "== $name =="
  make -C "$ROOT/src" clean
  make -C "$ROOT/src" -j"$JOBS" XCFLAGS="$xcflags"
  "$ROOT/tools/ci/run_stock_tests.sh" "$ROOT/src/luajit" --quiet $stock_tags
  "$ROOT/src/luajit" -e "require'ffi'; assert(2^31 == 2147483648)"
}

"$ROOT/tools/ci/m0_guardrails.sh"

run_combo "LJ_MT=0 JIT=1" "" ""
run_combo "LJ_MT=0 JIT=0" "-DLUAJIT_DISABLE_JIT" "-jit"
run_combo "LJ_MT=1 JIT=1" "-DLUAJIT_THREADSAFE" ""
run_combo "LJ_MT=1 JIT=0" "-DLUAJIT_THREADSAFE -DLUAJIT_DISABLE_JIT" "-jit"

echo "M0 matrix passed"
