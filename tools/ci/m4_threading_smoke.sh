#!/bin/sh
# Run M4 concurrent pure-compute threading smoke tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

LJ_M4_MT_SMOKE_THREADS=${LJ_M4_MT_SMOKE_THREADS:-8} \
  "$ROOT/src/luajit" -joff "$ROOT/tests/t-mt-smoke.lua"

echo "M4 threading smoke tests passed"
