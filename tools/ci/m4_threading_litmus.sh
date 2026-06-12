#!/bin/sh
# Run M4 Lua-visible threading synchronization litmus tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

LJ_M4_LITMUS_REPS=${LJ_M4_LITMUS_REPS:-100} \
  "$ROOT/src/luajit" -joff "$ROOT/tests/t-mt-litmus.lua"

echo "M4 threading litmus tests passed"
